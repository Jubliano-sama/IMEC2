#include "app_anchor.h"
#include "app_anchor_command_completion.h"
#include "app_anchor_ranging.h"
#include "app_anchor_survey_discovery.h"
#include "app_anchor_survey_runtime.h"

#include "app_radio_low_power_policy.h"
#include "app_board.h"
#include "app_config.h"
#include "app_discovery_assignment_policy.h"
#include "app_discovery_assignment_stack.h"
#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_survey_observability.h"
#include "app_gateway_command_ingress.h"
#include "app_gateway_command_lifecycle.h"
#include "app_high_debug.h"
#include "app_mesh_arbitration_zephyr.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_persistence.h"
#include "app_mesh_report.h"
#include "app_ml.h"
#include "app_node_comm.h"
#include "app_state.h"
#include "app_stack_workload_diag.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "discovery_assignment.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "route.h"
#include "serial_frame.h"
#include "status.h"
#include "survey.h"
#include "survey_gateway_transaction.h"
#include "survey_pair_lease.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_anchor, LOG_LEVEL_DBG);

#define ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS 1000u
#define GATEWAY_HOST_COMMAND_QUEUE_DEPTH 2u
#define GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS 8u
#define DISCOVERY_ASSIGNMENT_CLAIM_MAX_ATTEMPTS 8u
#define DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS 4u
#define DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS 4u
#define DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S 20u
#define GATEWAY_SURVEY_TRANSACTION_POLL_MS 50u

BUILD_ASSERT(UWB_DISCOVERY_SLOT_COUNT == SURVEY_GATEWAY_MAX_REPORTS,
             "gateway enumeration and survey capacities must both cover 50 anchors");
BUILD_ASSERT(UWB_DISCOVERY_SLOT_COUNT <= 50u,
             "gateway enumeration storage is intentionally capped at 50 anchors");
BUILD_ASSERT(SURVEY_GATEWAY_MAX_PEERS_PER_REPORT == SURVEY_REACH_MAX_ENTRIES,
             "anchor collection and gateway survey report caps must match");
BUILD_ASSERT(SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT <= 16u,
             "survey result accounting uses one bounded 16-bit sample mask");
#if DEVICE_ROLE == ROLE_ANCHOR
BUILD_ASSERT(REPORT_TX_QUEUE_DEPTH >= SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
             "mesh anchor report custody must cover every admitted survey sample");
#endif
BUILD_ASSERT(APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES <=
             APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_LIMIT_BYTES,
             "gateway discovery table publisher must stay below a 4 KiB stack frame");

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(UWB_RANGE_SCHEDULE_MAX_LEN <= UWB_MESH_MAX_FRAME_LEN,
             "post-wake route RX buffer must still fit normal ranging schedules");
BUILD_ASSERT(ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE >= 12288u,
             "mesh-route anchor scan needs the enlarged wake-frame stack");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_PRIORITY < ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
             "mesh route work must preempt low-duty anchor scan handoff");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_STACK_SIZE >= ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE,
             "mesh click handoff must have the full anchor sequence stack");
BUILD_ASSERT(ANCHOR_UWB_SCAN_BUSY_RETRY_MS > 0u,
             "blocked mesh route-test anchor scans must not spin at zero delay");
#endif

static struct k_work_delayable anchor_uwb_scan_work;
static struct k_work_delayable anchor_heartbeat_work;
static struct k_work_delayable anchor_reboot_work;
static struct k_work_delayable anchor_collection_result_work;
static struct k_work_delayable anchor_command_execute_work;
static struct k_work_delayable anchor_discovery_claim_work;
#if DEVICE_ROLE == ROLE_GATEWAY
static struct k_work_delayable gateway_discovery_assignment_finalize_work;
static struct k_work_delayable gateway_discovery_assignment_publish_work;
#endif

#define ANCHOR_COMMAND_DELIVERY_POLL_MS 5u

struct anchor_discovery_claim_pending {
    struct proto_packet command;
    uint32_t epoch;
    uint32_t delivery_handle;
    enum discovery_assignment_phase phase;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t hop_count;
    uint8_t attempt;
    bool active;
};

#if DEVICE_ROLE == ROLE_GATEWAY
enum gateway_discovery_assignment_stage {
    GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS = 0,
    GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS = 1,
};

struct gateway_discovery_assignment_state {
    struct proto_packet host_command;
    uint64_t anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint64_t ack_mask;
    uint32_t epoch;
    uint32_t claim_command_seq;
    uint32_t table_command_seq;
    uint32_t operation_deadline_ms;
    uint32_t command_budget_ms;
    uint32_t generation;
    size_t claim_count;
    size_t claim_count_at_round_start;
    enum gateway_discovery_assignment_stage stage;
    uint8_t claim_round;
    uint8_t table_round;
    uint8_t max_hop_count;
    uint16_t duplicate_count;
    bool round_open;
    bool budget_explicit;
    bool active;
};
#endif

static struct anchor_discovery_claim_pending anchor_discovery_claim_pending;
#if DEVICE_ROLE == ROLE_GATEWAY
static struct gateway_discovery_assignment_state gateway_discovery_assignment_state;
static struct app_discovery_assignment_work_guard
    gateway_discovery_assignment_publish_guard;
static uint32_t gateway_discovery_assignment_generation;
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
K_THREAD_STACK_DEFINE(anchor_uwb_scan_work_q_stack, ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE);
static struct k_work_q anchor_uwb_scan_work_q;
static const struct k_work_queue_config anchor_uwb_scan_work_q_config = {
    .name = "anchor_uwb_scan",
};
#endif
static uint16_t anchor_heartbeat_seq;
static uint32_t anchor_ch5_scan_debug_next_ms;
static uint32_t anchor_low_power_transition_failures;
static uint8_t anchor_uwb_scan_frame[UWB_MESH_MAX_FRAME_LEN];
static uint32_t anchor_heartbeat_interval_ms = ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS;
static bool anchor_reboot_pending;
static uint32_t anchor_reboot_deadline_ms;
static bool anchor_scan_recovery_gap_requested;
static struct survey_gateway_context gateway_survey_context;
static bool gateway_survey_active;
static uint32_t gateway_survey_operation_deadline_ms;
static bool gateway_survey_budget_explicit;
static struct k_work_delayable gateway_survey_work;
static struct survey_gateway_auto_context gateway_survey_auto;
static struct proto_packet gateway_survey_pending_command;
static bool gateway_survey_pending_command_valid;
static struct proto_packet gateway_survey_host_command;
static uint16_t gateway_survey_duplicate_count;
static uint16_t gateway_survey_pair_success_count;
static uint16_t gateway_survey_pair_failure_count;
static enum gateway_command_event_reason gateway_survey_terminal_failure_reason;
static uint16_t gateway_survey_pair_result_mask;
static uint16_t gateway_survey_pair_range_failure_count;
static bool gateway_survey_pair_observation_active;
static struct app_gateway_survey_observability_state
    gateway_survey_observability;
#if DEVICE_ROLE == ROLE_GATEWAY
struct gateway_survey_cleanup_delivery {
    struct survey_pair pair;
    uint64_t target_id;
    uint64_t absolute_deadline_ms;
    uint32_t client_token;
    uint32_t handle;
    uint16_t sequence;
    uint8_t peer_mask;
    bool prepared;
    bool submitted;
};
struct gateway_survey_result_preflight {
    struct node_transaction_key key;
    enum survey_gateway_transaction_result result;
    enum command_status status;
    uint8_t reason;
    bool valid;
};
static struct survey_gateway_transaction gateway_survey_transaction;
static struct gateway_survey_cleanup_delivery gateway_survey_cleanup;
static struct gateway_survey_result_preflight gateway_survey_result_preflight;
static uint32_t gateway_survey_transaction_client_token;
#endif
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
K_MSGQ_DEFINE(gateway_host_command_msgq,
              sizeof(struct app_gateway_command_ingress_item),
              GATEWAY_HOST_COMMAND_QUEUE_DEPTH,
              4);
static struct k_work_delayable gateway_host_command_work;
static struct k_work_delayable gateway_host_command_retry_work;
static uint8_t gateway_host_command_retry_round;
static uint32_t gateway_host_command_retry_started_ms;
static bool gateway_host_command_retry_pending;
static uint32_t gateway_host_command_next_admission_id;
static struct app_gateway_command_lifecycle gateway_host_command_lifecycle;
BUILD_ASSERT(GATEWAY_HOST_COMMAND_QUEUE_DEPTH <=
             APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS,
             "gateway command lifecycle must cover every queue slot");
#endif
static uint32_t anchor_collection_node_boot_counter;
static uint16_t anchor_collection_result_seq;
struct anchor_collection_result_pending {
    struct proto_packet command;
    struct command_result_id result_id;
    uint32_t collection_epoch_id;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool active;
    bool force_rediscovery_after_result;
    bool reboot_after_result;
    bool persistence_dirty;
    uint8_t persistence_retry_round;
};
static struct anchor_collection_result_pending anchor_collection_result_pending;
struct anchor_pending_command_options {
    enum command_response_mode response_mode;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint32_t collection_slot_seed;
    uint32_t command_expiry_s;
    uint16_t expected_node_count;
    bool collection_required;
};
struct anchor_pending_command_execution {
    struct proto_packet command;
    struct anchor_pending_command_options options;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint32_t received_at_ms;
    bool active;
};
static struct anchor_pending_command_execution anchor_pending_command_execution;
static struct app_mesh_command_orchestrator anchor_command_orchestrator;
#if defined(CONFIG_IMEC_ML_ANCHOR)
static uint32_t anchor_run_clicker_pair_survey(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_rx_ms);
#endif
static void anchor_set_uwb_busy(bool busy);
static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us);
static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);
static bool anchor_handle_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms);
static void anchor_uwb_scan_schedule_ms(uint32_t delay_ms);
static void anchor_reboot_work_handler(struct k_work *work);
static void anchor_collection_result_work_handler(struct k_work *work);
static void anchor_command_execute_work_handler(struct k_work *work);
static void anchor_schedule_reboot_after_command_result(void);
static void anchor_force_rediscovery_from_command(void);
static void gateway_survey_work_handler(struct k_work *work);
#if DEVICE_ROLE == ROLE_GATEWAY
static void gateway_survey_begin_cleanup(void);
static bool gateway_survey_cleanup_pending(void);
static int gateway_survey_cancel_take_active_delivery(
    enum node_transaction_action *action);
#endif
static void gateway_survey_auto_finish(void);
static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason);
static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id);
static void anchor_heartbeat_work_handler(struct k_work *work);

static bool anchor_discovery_assignment_required(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           !IS_ENABLED(CONFIG_IMEC_ML_ANCHOR) &&
           !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
           IMEC_HIGH_DEBUG_ANCHOR_SLOT_ENABLED == 0;
}

static int anchor_append_discovery_assignment_status(uint8_t *payload,
                                                     size_t payload_cap,
                                                     size_t *payload_len)
{
    static const uint8_t unprovisioned[] = "UNPROVISIONED";
    uint32_t epoch = 0u;
    uint8_t slot = 0u;
    uint8_t slot_count = 0u;

    if (!anchor_discovery_assignment_required()) {
        return PROTO_OK;
    }
    if (local_anchor_discovery_assignment_get(&epoch, &slot, &slot_count)) {
        return tlv_append_u32(payload,
                              payload_cap,
                              payload_len,
                              TLV_DISCOVERY_ASSIGNMENT_EPOCH,
                              epoch);
    }
    return tlv_append_bytes(payload,
                            payload_cap,
                            payload_len,
                            TLV_ERROR_DETAIL,
                            unprovisioned,
                            (uint8_t)(sizeof(unprovisioned) - 1u));
}

static int anchor_enter_low_power(enum app_radio_low_power_mode mode,
                                  const char *reason)
{
    struct app_radio_low_power_policy policy;
    enum app_radio_low_power_action action;
    const char *mode_name = app_radio_low_power_mode_name(mode);
    int first_ret;
    int recovery_ret;
    int retry_ret = 0;
    int final_ret;
    bool retry_attempted = false;

    app_radio_low_power_policy_init(&policy, mode);
    first_ret = mode == APP_RADIO_LOW_POWER_IDLE ?
                dwm3000_driver_idle() : dwm3000_driver_standby();
    action = app_radio_low_power_policy_note_transition(&policy, first_ret);
    if (action == APP_RADIO_LOW_POWER_COMPLETE) {
        return 0;
    }

    recovery_ret = dwm3000_driver_force_recovery();
    action = app_radio_low_power_policy_note_recovery(&policy, recovery_ret);
    if (action == APP_RADIO_LOW_POWER_RETRY) {
        retry_attempted = true;
        retry_ret = mode == APP_RADIO_LOW_POWER_IDLE ?
                    dwm3000_driver_idle() : dwm3000_driver_standby();
        action = app_radio_low_power_policy_note_transition(&policy, retry_ret);
        if (action == APP_RADIO_LOW_POWER_COMPLETE) {
            LOG_WRN("anchor DWM3000 %s recovered after transition failure: reason=%s first_ret=%d",
                    mode_name,
                    reason,
                    first_ret);
            status_debug_printf("DBG_ANCHOR_LOW_POWER_RECOVERED mode=%s first=%d\n",
                                mode_name,
                                first_ret);
            return 0;
        }
    }

    final_ret = recovery_ret < 0 ? recovery_ret : retry_ret;
    if (anchor_low_power_transition_failures != UINT32_MAX) {
        anchor_low_power_transition_failures++;
    }
    LOG_ERR("anchor DWM3000 %s failed after bounded recovery: reason=%s first_ret=%d recovery_ret=%d retry_attempted=%u retry_ret=%d failures=%u",
            mode_name,
            reason,
            first_ret,
            recovery_ret,
            retry_attempted ? 1u : 0u,
            retry_ret,
            anchor_low_power_transition_failures);
    status_debug_printf("DBG_ANCHOR_LOW_POWER_FAILED mode=%s ret=%d count=%u\n",
                        mode_name,
                        final_ret,
                        anchor_low_power_transition_failures);
    return final_ret;
}

static uint32_t anchor_collection_node_boot_id(void)
{
    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = sys_rand32_get();
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = k_uptime_get_32();
        }
        if (anchor_collection_node_boot_counter == 0u) {
            anchor_collection_node_boot_counter = 1u;
        }
    }
    return anchor_collection_node_boot_counter;
}

static uint16_t anchor_next_collection_result_seq(void)
{
    anchor_collection_result_seq++;
    if (anchor_collection_result_seq == 0u) {
        anchor_collection_result_seq = 1u;
    }
    return anchor_collection_result_seq;
}

static int anchor_submit_command_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    const struct command_result_id *collection_result_id,
    uint32_t collection_epoch_id,
    uint32_t *delivery_handle_out)
{
    struct mesh_outbound outbound = {0};
    uint32_t delivery_handle = 0u;
    uint32_t client_token;
    uint64_t absolute_deadline_ms;
    uint64_t now_ms;
    size_t payload_len = 0u;
    uint32_t session_id;
    uint16_t seq;
    bool diagnostic;
    int ret;

    if (command == NULL) {
        return -EINVAL;
    }

    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     command_id,
                                     status,
                                     reason);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    if (status == COMMAND_OK && command_id == CMD_GET_STATUS) {
        ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
        if (ret == PROTO_OK) {
            ret = anchor_append_discovery_assignment_status(outbound.payload,
                                                            sizeof(outbound.payload),
                                                            &payload_len);
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    if (collection_result_id != NULL) {
        ret = gateway_command_append_collection_result_identity(outbound.payload,
                                                               sizeof(outbound.payload),
                                                               &payload_len,
                                                               collection_result_id,
                                                               collection_epoch_id);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        session_id = collection_result_id->command_seq;
        seq = collection_result_id->result_seq;
    } else {
        session_id = command->session_id;
        seq = command->seq;
    }

    diagnostic = (command->flags & FLAG_DIAGNOSTIC) != 0u;
    outbound.payload_len = (uint16_t)payload_len;
    ret = app_mesh_command_orchestrator_anchor_result(command,
                                                       DEVICE_ID,
                                                       GATEWAY_ID,
                                                       diagnostic,
                                                       &outbound);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.packet.session_id = session_id;
    outbound.packet.seq = seq;

    now_ms = (uint64_t)k_uptime_get();
    absolute_deadline_ms = UINT64_MAX - now_ms <
                           GATEWAY_COMMAND_RESULT_TIMEOUT_MS ?
                           UINT64_MAX :
                           now_ms + GATEWAY_COMMAND_RESULT_TIMEOUT_MS;
    client_token = ((uint32_t)command_id << 16) | seq;
    ret = app_node_comm_submit_protocol_response(
        &outbound,
        absolute_deadline_ms,
        client_token,
        delivery_handle_out == NULL ? NULL : &delivery_handle);
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
        if (delivery_handle_out != NULL) {
            *delivery_handle_out = delivery_handle;
        }
    }
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=uwb_mesh command=0x%04x status=%s reason=%u ret=%d",
                         (unsigned int)command_id,
                         command_status_name(status),
                         reason,
                         ret);
    return ret;
}

static int anchor_send_command_result(const struct proto_packet *command,
                                      enum command_id command_id,
                                      enum command_status status,
                                      uint8_t reason,
                                      const struct command_result_id *collection_result_id,
                                      uint32_t collection_epoch_id)
{
    return anchor_submit_command_result(command,
                                        command_id,
                                        status,
                                        reason,
                                        collection_result_id,
                                        collection_epoch_id,
                                        NULL);
}

static uint8_t anchor_command_completion_actions(
    enum command_status status,
    bool force_rediscovery_after_result,
    bool reboot_after_result)
{
    uint8_t actions = 0u;

    if (status != COMMAND_OK) {
        return 0u;
    }
    if (force_rediscovery_after_result) {
        actions |= APP_ANCHOR_COMMAND_COMPLETION_FORCE_REDISCOVERY;
    }
    if (reboot_after_result) {
        actions |= APP_ANCHOR_COMMAND_COMPLETION_REBOOT;
    }
    return actions;
}

static uint8_t anchor_discovery_gateway_hop_count(void)
{
    const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);

    if (selected == NULL || selected->hop_count == UINT8_MAX) {
        return 0u;
    }
    /* Route candidates count intermediate relays; assignment reports RF hops. */
    return selected->hop_count + 1u;
}

static uint32_t anchor_discovery_response_delay_ms(
    const struct anchor_discovery_claim_pending *pending)
{
    uint32_t delay_ms = DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;

    if (pending == NULL ||
        discovery_assignment_response_delay_ms(pending->slot,
                                               pending->slot_count,
                                               pending->hop_count,
                                               pending->attempt,
                                               sys_rand32_get(),
                                               &delay_ms) != PROTO_OK) {
        return DISCOVERY_ASSIGNMENT_RETRY_BASE_MS;
    }
    return delay_ms;
}

static int anchor_send_discovery_response(
    const struct anchor_discovery_claim_pending *pending,
    uint32_t *delivery_handle_out)
{
    struct mesh_outbound outbound = {0};
    uint64_t hash;
    size_t payload_len = 0u;
    int ret;

    if (pending == NULL || pending->epoch == 0u) {
        return -EINVAL;
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    ret = mesh_append_command_result(outbound.payload,
                                     sizeof(outbound.payload),
                                     &payload_len,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     COMMAND_OK,
                                     0u);
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
             outbound.payload,
             sizeof(outbound.payload),
             &payload_len,
             pending->phase,
             pending->epoch);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_claim_hash(outbound.payload,
                                                     sizeof(outbound.payload),
                                                     &payload_len,
                                                     hash);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound.payload,
                            sizeof(outbound.payload),
                            &payload_len,
                            TLV_HOP_COUNT,
                            pending->hop_count);
    }
    if (ret == PROTO_OK) {
        ret = mesh_init_command_result(&outbound.packet,
                                       DEVICE_ID,
                                       GATEWAY_ID,
                                       pending->command.session_id,
                                       pending->command.seq,
                                       (uint8_t)payload_len,
                                       true);
    }
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint16_t)payload_len;
    ret = app_node_comm_submit_protocol_response(
        &outbound,
        (uint64_t)k_uptime_get() + GATEWAY_COMMAND_RESULT_TIMEOUT_MS,
        ((uint32_t)CMD_ASSIGN_DISCOVERY_SLOTS << 16) |
            pending->command.seq,
        delivery_handle_out);
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE phase=%u epoch=%u hash=0x%016llx hop=%u slot=%u attempt=%u ret=%d\n",
                        pending->phase,
                        pending->epoch,
                        (unsigned long long)hash,
                        pending->hop_count,
                        pending->slot,
                        pending->attempt + 1u,
                        ret);
    if (ret == 0) {
        HIGH_DEBUG_COUNTER_INC(command_result_tx);
    }
    return ret;
}

static void anchor_discovery_claim_work_handler(struct k_work *work)
{
    struct node_comm_terminal_event event;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_discovery_claim_pending.active) {
        return;
    }
    if (anchor_discovery_claim_pending.delivery_handle != 0u) {
        if (!app_node_comm_take_delivery_event_for(
                anchor_discovery_claim_pending.delivery_handle, &event)) {
            (void)mesh_route_work_reschedule(
                &anchor_discovery_claim_work,
                ANCHOR_COMMAND_DELIVERY_POLL_MS);
            return;
        }
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
            status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_DELIVERED phase=%u epoch=%u attempts=%u\n",
                                anchor_discovery_claim_pending.phase,
                                anchor_discovery_claim_pending.epoch,
                                event.attempts_started);
            anchor_discovery_claim_pending.delivery_handle = 0u;
            anchor_discovery_claim_pending.active = false;
            return;
        }
        LOG_ERR("anchor discovery-slot response failed: phase=%u epoch=%u reason=%u attempts=%u",
                anchor_discovery_claim_pending.phase,
                anchor_discovery_claim_pending.epoch,
                (unsigned int)event.reason,
                event.attempts_started);
        status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_FAILED phase=%u epoch=%u attempts=%u reason=%u\n",
                            anchor_discovery_claim_pending.phase,
                            anchor_discovery_claim_pending.epoch,
                            event.attempts_started,
                            (unsigned int)event.reason);
        anchor_discovery_claim_pending.delivery_handle = 0u;
        anchor_discovery_claim_pending.active = false;
        return;
    }
    anchor_discovery_claim_pending.hop_count =
        anchor_discovery_gateway_hop_count();
    ret = anchor_send_discovery_response(
        &anchor_discovery_claim_pending,
        &anchor_discovery_claim_pending.delivery_handle);
    if (ret < 0) {
        LOG_ERR("anchor discovery-slot response admission failed: phase=%u epoch=%u ret=%d",
                anchor_discovery_claim_pending.phase,
                anchor_discovery_claim_pending.epoch,
                ret);
        anchor_discovery_claim_pending.active = false;
    }
}

static int anchor_schedule_discovery_response(
    const struct proto_packet *command,
    uint32_t epoch,
    enum discovery_assignment_phase phase,
    uint8_t slot,
    uint8_t slot_count)
{
    uint64_t hash;
    uint32_t delay_ms;

    if (command == NULL || epoch == 0u ||
        (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) ||
        slot_count == 0u || slot >= slot_count) {
        return -EINVAL;
    }
    if (anchor_discovery_claim_pending.active &&
        anchor_discovery_claim_pending.epoch == epoch &&
        anchor_discovery_claim_pending.phase == phase &&
        anchor_discovery_claim_pending.command.session_id ==
            command->session_id &&
        anchor_discovery_claim_pending.command.seq == command->seq) {
        return 0;
    }
    if (anchor_discovery_claim_pending.delivery_handle != 0u) {
        (void)app_node_comm_abandon_delivery(
            anchor_discovery_claim_pending.delivery_handle);
    }
    hash = discovery_assignment_hash(DEVICE_ID);
    anchor_discovery_claim_pending.command = *command;
    anchor_discovery_claim_pending.epoch = epoch;
    anchor_discovery_claim_pending.delivery_handle = 0u;
    anchor_discovery_claim_pending.phase = phase;
    anchor_discovery_claim_pending.slot = slot;
    anchor_discovery_claim_pending.slot_count = slot_count;
    anchor_discovery_claim_pending.hop_count =
        anchor_discovery_gateway_hop_count();
    anchor_discovery_claim_pending.attempt = 0u;
    anchor_discovery_claim_pending.active = true;
    delay_ms = anchor_discovery_response_delay_ms(
        &anchor_discovery_claim_pending);
    (void)mesh_route_work_reschedule(&anchor_discovery_claim_work, delay_ms);
    status_debug_printf("DBG_DISCOVERY_SLOT_RESPONSE_SCHEDULED phase=%u epoch=%u delay=%u slot=%u hop=%u hash=0x%016llx\n",
                        phase,
                        epoch,
                        delay_ms,
                        slot,
                        anchor_discovery_claim_pending.hop_count,
                        (unsigned long long)hash);
    return 0;
}

static int anchor_schedule_discovery_claim(const struct proto_packet *command,
                                           uint32_t epoch)
{
    uint8_t slot = (uint8_t)(discovery_assignment_hash(DEVICE_ID) %
                             UWB_DISCOVERY_SLOT_COUNT);

    return anchor_schedule_discovery_response(command,
                                              epoch,
                                              DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
                                              slot,
                                              UWB_DISCOVERY_SLOT_COUNT);
}

static void anchor_collection_result_clear(void)
{
    memset(&anchor_collection_result_pending, 0, sizeof(anchor_collection_result_pending));
}

static int anchor_collection_result_persist(uint32_t delay_ms)
{
    struct app_mesh_collection_result_snapshot snapshot = {
        .version = APP_MESH_COLLECTION_RESULT_SNAPSHOT_VERSION,
        .local_id = DEVICE_ID,
        .gateway_id = GATEWAY_ID,
        .valid = anchor_collection_result_pending.active,
    };

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return -EINVAL;
    }

    snapshot.command = anchor_collection_result_pending.command;
    snapshot.result_id = anchor_collection_result_pending.result_id;
    snapshot.collection_epoch_id = anchor_collection_result_pending.collection_epoch_id;
    snapshot.delay_ms = delay_ms == 0u ? 1u : delay_ms;
    snapshot.command_id = anchor_collection_result_pending.command_id;
    snapshot.status = anchor_collection_result_pending.status;
    snapshot.reason = anchor_collection_result_pending.reason;
    snapshot.force_rediscovery_after_result =
        anchor_collection_result_pending.force_rediscovery_after_result;
    snapshot.reboot_after_result =
        anchor_collection_result_pending.reboot_after_result;

    return app_mesh_persistence_save_collection_result(&snapshot);
}

static int anchor_collection_result_restore(void)
{
    struct app_mesh_collection_result_snapshot snapshot;
    uint32_t delay_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || anchor_collection_result_pending.active) {
        return -EBUSY;
    }

    ret = app_mesh_persistence_restore_collection_result(&snapshot);
    if (ret < 0) {
        return ret;
    }
    if (!snapshot.valid) {
        return 0;
    }
    if (snapshot.local_id != DEVICE_ID ||
        snapshot.gateway_id != GATEWAY_ID ||
        snapshot.result_id.node_id != DEVICE_ID ||
        snapshot.result_id.gateway_id != GATEWAY_ID ||
        (snapshot.command.dst_id != DEVICE_ID &&
         snapshot.command.dst_id != MESH_BROADCAST_ID)) {
        app_mesh_persistence_clear_collection_result();
        return -EINVAL;
    }

    anchor_collection_result_pending.command = snapshot.command;
    anchor_collection_result_pending.result_id = snapshot.result_id;
    anchor_collection_result_pending.collection_epoch_id = snapshot.collection_epoch_id;
    anchor_collection_result_pending.command_id = snapshot.command_id;
    anchor_collection_result_pending.status = snapshot.status;
    anchor_collection_result_pending.reason = snapshot.reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        snapshot.force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result =
        snapshot.reboot_after_result;
    anchor_collection_result_pending.active = true;
    anchor_collection_result_pending.persistence_dirty = false;
    anchor_collection_result_pending.persistence_retry_round = 0u;

    if (anchor_collection_node_boot_counter == 0u) {
        anchor_collection_node_boot_counter = snapshot.result_id.node_boot_counter;
    }
    if (anchor_collection_result_seq < snapshot.result_id.result_seq) {
        anchor_collection_result_seq = snapshot.result_id.result_seq;
    }

    delay_ms = snapshot.delay_ms == 0u ? 1u : snapshot.delay_ms;
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));
    LOG_INF("anchor collection command result restored: cmd=0x%04x delay_ms=%u",
            (unsigned int)anchor_collection_result_pending.command_id,
            delay_ms);
    return 0;
}

static void anchor_collection_result_work_handler(struct k_work *work)
{
    struct anchor_collection_result_pending pending;
    uint32_t delivery_handle = 0u;
    uint8_t completion_actions;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_collection_result_pending.active) {
        return;
    }

    if (anchor_collection_result_pending.persistence_dirty) {
        uint32_t retry_ms;

        ret = anchor_collection_result_persist(1u);
        if (ret < 0) {
            retry_ms = discovery_assignment_retry_backoff_ms(
                anchor_collection_result_pending.persistence_retry_round,
                sys_rand32_get());
            if (anchor_collection_result_pending.persistence_retry_round < UINT8_MAX) {
                anchor_collection_result_pending.persistence_retry_round++;
            }
            LOG_WRN("anchor collection result persistence retry: ret=%d round=%u delay_ms=%u",
                    ret,
                    anchor_collection_result_pending.persistence_retry_round,
                    retry_ms);
            (void)k_work_reschedule(&anchor_collection_result_work,
                                    K_MSEC(retry_ms));
            return;
        }
        anchor_collection_result_pending.persistence_dirty = false;
        anchor_collection_result_pending.persistence_retry_round = 0u;
    }

    pending = anchor_collection_result_pending;
    anchor_collection_result_clear();
    completion_actions = anchor_command_completion_actions(
        pending.status,
        pending.force_rediscovery_after_result,
        pending.reboot_after_result);

    ret = anchor_submit_command_result(
        &pending.command,
        pending.command_id,
        pending.status,
        pending.reason,
        &pending.result_id,
        pending.collection_epoch_id,
        completion_actions == 0u ? NULL : &delivery_handle);
    if (ret < 0) {
        LOG_WRN("anchor collection command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)pending.command_id,
                pending.status,
                ret);
        pending.active = true;
        pending.persistence_dirty = true;
        anchor_collection_result_pending = pending;
        (void)k_work_reschedule(&anchor_collection_result_work,
                                K_MSEC(COLLECTION_RETRY_ROUND_0_MS));
        (void)anchor_collection_result_persist(COLLECTION_RETRY_ROUND_0_MS);
        return;
    }
    if (completion_actions != 0u) {
        ret = app_anchor_command_completion_watch(delivery_handle,
                                                  pending.command_id,
                                                  completion_actions);
        if (ret < 0) {
            (void)app_node_comm_abandon_delivery(delivery_handle);
            pending.active = true;
            pending.persistence_dirty = true;
            anchor_collection_result_pending = pending;
            (void)k_work_reschedule(&anchor_collection_result_work,
                                    K_MSEC(COLLECTION_RETRY_ROUND_0_MS));
            (void)anchor_collection_result_persist(
                COLLECTION_RETRY_ROUND_0_MS);
            LOG_WRN("anchor command completion admission failed: cmd=0x%04x ret=%d",
                    (unsigned int)pending.command_id, ret);
            return;
        }
    }

    app_mesh_persistence_clear_collection_result();
    LOG_INF("anchor collection command result sent: cmd=0x%04x status=%u reason=%u",
            (unsigned int)pending.command_id,
            pending.status,
            pending.reason);
}

static int anchor_schedule_collection_command_result(
    const struct proto_packet *command,
    const struct gateway_command_options *options,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason,
    bool force_rediscovery_after_result,
    bool reboot_after_result)
{
    uint32_t now_ms;
    uint32_t due_ms;
    uint32_t delay_ms;

    if (command == NULL || options == NULL || !options->collection_required) {
        return -EINVAL;
    }
    if (anchor_collection_result_pending.active) {
        LOG_WRN("anchor collection command result already pending: cmd=0x%04x new=0x%04x",
                (unsigned int)anchor_collection_result_pending.command_id,
                (unsigned int)command_id);
        return -EBUSY;
    }

    now_ms = k_uptime_get_32();
    due_ms = gateway_command_collection_initial_due_ms(now_ms,
                                                       DEVICE_ID,
                                                       options->command_seq,
                                                       options->collection_slot_seed,
                                                       options->expected_node_count);
    delay_ms = due_ms - now_ms;
    if (delay_ms == 0u) {
        delay_ms = 1u;
    }

    anchor_collection_result_pending.command = *command;
    anchor_collection_result_pending.collection_epoch_id = options->collection_epoch_id;
    anchor_collection_result_pending.result_id.gateway_id = GATEWAY_ID;
    anchor_collection_result_pending.result_id.gateway_epoch =
        (uint16_t)mesh_runtime.upstream.current_epoch;
    anchor_collection_result_pending.result_id.command_seq = options->command_seq;
    anchor_collection_result_pending.result_id.node_id = DEVICE_ID;
    anchor_collection_result_pending.result_id.node_boot_counter =
        anchor_collection_node_boot_id();
    anchor_collection_result_pending.result_id.result_seq =
        anchor_next_collection_result_seq();
    anchor_collection_result_pending.command_id = command_id;
    anchor_collection_result_pending.status = status;
    anchor_collection_result_pending.reason = reason;
    anchor_collection_result_pending.force_rediscovery_after_result =
        force_rediscovery_after_result;
    anchor_collection_result_pending.reboot_after_result = reboot_after_result;
    anchor_collection_result_pending.active = true;
    anchor_collection_result_pending.persistence_dirty = true;
    anchor_collection_result_pending.persistence_retry_round = 0u;
    if (anchor_collection_result_persist(delay_ms) == 0) {
        anchor_collection_result_pending.persistence_dirty = false;
    }
    (void)k_work_reschedule(&anchor_collection_result_work, K_MSEC(delay_ms));

    LOG_INF("anchor collection command result scheduled: cmd=0x%04x due_ms=%u delay_ms=%u",
            (unsigned int)command_id,
            due_ms,
            delay_ms);
    high_debug_log_event("COMMAND_RESULT_TX",
                         "transport=collection-scheduled command=0x%04x command_seq=%u result_seq=%u collection=%u due_ms=%u delay_ms=%u",
                         (unsigned int)command_id,
                         anchor_collection_result_pending.result_id.command_seq,
                         anchor_collection_result_pending.result_id.result_seq,
                         anchor_collection_result_pending.collection_epoch_id,
                         due_ms,
                         delay_ms);
    return 0;
}

static void anchor_reboot_work_handler(struct k_work *work)
{
    uint32_t now_ms;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_reboot_pending) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (mesh_relay_tx_active(&mesh_runtime) &&
        !uptime_deadline_reached(now_ms, anchor_reboot_deadline_ms)) {
        (void)k_work_reschedule(&anchor_reboot_work,
                                K_MSEC(ANCHOR_REBOOT_RESULT_POLL_MS));
        return;
    }

    LOG_INF("anchor rebooting after command-result drain");
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static void anchor_schedule_reboot_after_command_result(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    anchor_reboot_pending = true;
    anchor_reboot_deadline_ms = k_uptime_get_32() + ANCHOR_REBOOT_RESULT_MAX_WAIT_MS;
    (void)k_work_reschedule(&anchor_reboot_work, K_MSEC(ANCHOR_REBOOT_RESULT_DRAIN_MS));
}

static uint16_t anchor_next_heartbeat_seq(void)
{
    anchor_heartbeat_seq++;
    if (anchor_heartbeat_seq == 0u) {
        anchor_heartbeat_seq = 1u;
    }
    return anchor_heartbeat_seq;
}

static int anchor_send_heartbeat(void)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    ret = append_anchor_status_tlvs(outbound.payload, sizeof(outbound.payload), &payload_len);
    if (ret == PROTO_OK) {
        ret = anchor_append_discovery_assignment_status(outbound.payload,
                                                        sizeof(outbound.payload),
                                                        &payload_len);
    }
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = report_init_anchor_heartbeat_packet(&outbound.packet,
                                              DEVICE_ID,
                                              GATEWAY_ID,
                                              nonzero_uptime_session_id(),
                                              anchor_next_heartbeat_seq(),
                                              (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }
    outbound.payload_len = (uint8_t)payload_len;

    return mesh_start_tracked_tx(&outbound, "anchor-heartbeat");
}

static void anchor_heartbeat_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR && anchor_heartbeat_enabled) {
        (void)k_work_reschedule(&anchor_heartbeat_work, K_MSEC(delay_ms));
    }
}

static void anchor_heartbeat_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_heartbeat_enabled) {
        return;
    }
    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_discovery_is_pending() ||
        mesh_relay_tx_active(&mesh_runtime)) {
        anchor_heartbeat_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    ret = anchor_send_heartbeat();
    if (ret < 0) {
        LOG_WRN("anchor heartbeat TX failed: ret=%d interval_ms=%u", ret,
                anchor_heartbeat_interval_ms);
    } else {
        LOG_INF("anchor heartbeat queued: interval_ms=%u seq=%u",
                anchor_heartbeat_interval_ms,
                anchor_heartbeat_seq);
    }

    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
}

static int anchor_start_heartbeat_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              ANCHOR_HEARTBEAT_DEFAULT_INTERVAL_MS,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_HEARTBEAT_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_HEARTBEAT_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_heartbeat_interval_ms = interval_ms;
    anchor_heartbeat_enabled = true;
    anchor_heartbeat_schedule(anchor_heartbeat_interval_ms);
    return 0;
}

static void anchor_stop_heartbeat(void)
{
    anchor_heartbeat_enabled = false;
    (void)k_work_cancel_delayable(&anchor_heartbeat_work);
}

static int command_find_u8_tlv(const uint8_t *payload,
                               size_t payload_len,
                               uint8_t type,
                               uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = tlv_value[0];
    return PROTO_OK;
}

static int command_find_u32_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u32_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u16_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

static int command_find_u64_tlv(const uint8_t *payload,
                                size_t payload_len,
                                uint8_t type,
                                uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint64_t)) {
        return PROTO_ERR_MALFORMED;
    }

    *value = proto_get_u64_le(tlv_value);
    return PROTO_OK;
}

static int anchor_set_led_pattern_from_command(const uint8_t *payload,
                                                size_t payload_len,
                                                uint8_t *reason)
{
    uint8_t pattern_id = 0u;
    int ret;

    ret = command_find_u8_tlv(payload, payload_len, TLV_LED_PATTERN_ID, &pattern_id);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }

    switch ((enum status_pattern)pattern_id) {
    case STATUS_PATTERN_OFF:
        status_leds_set(false, false, false);
        break;
    case STATUS_PATTERN_BLUE_PULSE:
    case STATUS_PATTERN_BLUE_CHASE:
        status_leds_set(false, false, true);
        break;
    case STATUS_PATTERN_GREEN_SOLID:
    case STATUS_PATTERN_GREEN_SLOW_BLINK:
        status_leds_set(false, true, false);
        break;
    case STATUS_PATTERN_AMBER_BLINK_ONCE:
    case STATUS_PATTERN_AMBER_SLOW_BLINK:
        status_leds_set(true, true, false);
        break;
    case STATUS_PATTERN_RED_BLINK_CODE:
        status_leds_set(true, false, false);
        break;
    default:
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    LOG_INF("anchor LED pattern command applied: pattern=%u", pattern_id);
    return 0;
}

static int anchor_set_route_from_command(const uint8_t *payload,
                                          size_t payload_len,
                                          uint8_t *reason)
{
    struct route_candidate candidate = {0};
    uint8_t hop_count = 0u;
    uint8_t quality = 0u;
    int ret;

    ret = command_find_u64_tlv(payload, payload_len, TLV_NEXT_HOP_ID, &candidate.next_hop_id);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u32_tlv(payload, payload_len, TLV_ROUTE_EPOCH, &candidate.route_epoch);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_HOP_COUNT, &hop_count);
    if (ret != PROTO_OK) {
        goto malformed;
    }
    ret = command_find_u8_tlv(payload, payload_len, TLV_QUALITY, &quality);
    if (ret != PROTO_OK) {
        goto malformed;
    }

    candidate.gateway_id = GATEWAY_ID;
    candidate.hop_count = hop_count;
    candidate.link_quality = quality;
    candidate.last_seen_ms = k_uptime_get_32();
    candidate.valid = true;

    ret = route_upsert_candidate(&mesh_runtime.upstream, &candidate);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return ret == PROTO_ERR_NO_SPACE ? -ENOSPC : -EINVAL;
    }
    mesh_relay_note_route_discovery_ready(&mesh_runtime, GATEWAY_ID);

    LOG_INF("anchor route command applied: next=0x%016llx epoch=%u hop_count=%u quality=%u",
            (unsigned long long)candidate.next_hop_id,
            candidate.route_epoch,
            hop_count,
            quality);
    return 0;

malformed:
    if (reason != NULL) {
        *reason = (uint8_t)(-ret);
    }
    return -EINVAL;
}

static void anchor_clear_route_from_command(void)
{
    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    LOG_INF("anchor route command cleared upstream route state");
}

static void anchor_force_rediscovery_from_command(void)
{
    int ret;

    mesh_relay_clear_routes_preserve_epoch(&mesh_runtime);
    ret = mesh_request_route(GATEWAY_ID, "forced-rediscovery");
    if (ret < 0 && ret != -EAGAIN) {
        LOG_WRN("anchor forced rediscovery request failed: ret=%d", ret);
    } else {
        LOG_INF("anchor forced rediscovery requested");
    }
}

static int anchor_set_scan_duty_from_command(const uint8_t *payload,
                                              size_t payload_len,
                                              uint8_t *reason)
{
    uint32_t interval_ms = 0u;
    int ret;

    ret = gateway_command_extract_duration_ms(payload,
                                              payload_len,
                                              anchor_uwb_scan_interval_ms,
                                              &interval_ms);
    if (ret != PROTO_OK) {
        if (reason != NULL) {
            *reason = (uint8_t)(-ret);
        }
        return -EINVAL;
    }
    if (interval_ms < ANCHOR_UWB_SCAN_MIN_INTERVAL_MS ||
        interval_ms > ANCHOR_UWB_SCAN_MAX_INTERVAL_MS) {
        if (reason != NULL) {
            *reason = (uint8_t)(-PROTO_ERR_MALFORMED);
        }
        return -EINVAL;
    }

    anchor_uwb_scan_interval_ms = interval_ms;
    if (!anchor_uwb_window_active()) {
        anchor_uwb_scan_schedule_ms(anchor_uwb_scan_interval_ms);
    }
    LOG_INF("anchor UWB scan duty command applied: interval_ms=%u min_ms=%u max_ms=%u awake_us=%u",
            anchor_uwb_scan_interval_ms,
            (unsigned int)ANCHOR_UWB_SCAN_MIN_INTERVAL_MS,
            (unsigned int)ANCHOR_UWB_SCAN_MAX_INTERVAL_MS,
            ANCHOR_UWB_IDLE_SCAN_AWAKE_US);
    return 0;
}

static void anchor_preempt_for_survey_discovery(uint32_t survey_id)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    (void)k_work_cancel_delayable(&anchor_uwb_scan_work);
    if (anchor_uwb_session.state != UWB_ANCHOR_IDLE) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
        LOG_INF("survey discovery preempted pending click epoch: survey=%u", survey_id);
    }
    app_node_comm_stop_role_scan();
}

static void anchor_pending_options_to_gateway(
    const struct anchor_pending_command_options *pending,
    struct gateway_command_options *options)
{
    memset(options, 0, sizeof(*options));
    options->scope = CMD_SCOPE_ALL_REGISTERED;
    options->response_mode = pending->response_mode;
    options->command_seq = pending->command_seq;
    options->collection_epoch_id = pending->collection_epoch_id;
    options->collection_slot_seed = pending->collection_slot_seed;
    options->command_expiry_s = pending->command_expiry_s;
    options->expected_node_count = pending->expected_node_count;
    options->collection_required = pending->collection_required;
    options->flood_required = true;
}

static int anchor_apply_discovery_assignment_command(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    struct discovery_assignment_entry entries[UWB_DISCOVERY_SLOT_COUNT];
    enum discovery_assignment_phase phase = 0;
    enum app_discovery_assignment_table_decision table_decision;
    uint32_t epoch = 0u;
    size_t entry_count = 0u;
    uint8_t slot_count = 0u;
    int ret;

    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (phase == DISCOVERY_ASSIGNMENT_PHASE_CLAIM) {
        enum app_discovery_assignment_claim_decision claim_decision =
            local_anchor_discovery_assignment_note_claim(epoch);

        if (claim_decision == APP_DISCOVERY_ASSIGNMENT_CLAIM_INVALID) {
            return -EINVAL;
        }
        if (claim_decision == APP_DISCOVERY_ASSIGNMENT_CLAIM_IGNORE_STALE) {
            status_debug_printf("DBG_DISCOVERY_SLOT_STALE phase=claim epoch=%u state=%s\n",
                                epoch,
                                app_discovery_assignment_provisioning_state_name(
                                    local_anchor_discovery_assignment_provisioning_state()));
            LOG_WRN("anchor ignored stale discovery assignment claim: epoch=%u",
                    epoch);
            return 0;
        }
        return anchor_schedule_discovery_claim(packet, epoch);
    }
    if (phase != DISCOVERY_ASSIGNMENT_PHASE_TABLE) {
        return -EINVAL;
    }
    ret = discovery_assignment_parse_table_tlvs(payload,
                                                payload_len,
                                                entries,
                                                ARRAY_SIZE(entries),
                                                &entry_count,
                                                &slot_count);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    table_decision = local_anchor_discovery_assignment_note_table(epoch);
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_INVALID) {
        return -EINVAL;
    }
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_IGNORE_STALE) {
        status_debug_printf("DBG_DISCOVERY_SLOT_STALE phase=table epoch=%u state=%s\n",
                            epoch,
                            app_discovery_assignment_provisioning_state_name(
                                local_anchor_discovery_assignment_provisioning_state()));
        LOG_WRN("anchor ignored stale discovery assignment table: epoch=%u",
                epoch);
        return 0;
    }
    if (table_decision == APP_DISCOVERY_ASSIGNMENT_TABLE_LATE_CLAIM) {
        status_debug_printf("DBG_DISCOVERY_SLOT_LATE_CLAIM epoch=%u state=%s count=%u\n",
                            epoch,
                            app_discovery_assignment_provisioning_state_name(
                                local_anchor_discovery_assignment_provisioning_state()),
                            (unsigned int)entry_count);
        LOG_WRN("anchor received discovery table before claim epoch: epoch=%u count=%u; retaining committed assignment and late-claiming",
                epoch,
                (unsigned int)entry_count);
        return anchor_schedule_discovery_claim(packet, epoch);
    }

    (void)k_work_cancel_delayable(&anchor_discovery_claim_work);
    anchor_discovery_claim_pending.active = false;
    for (size_t i = 0u; i < entry_count; i++) {
        if (entries[i].anchor_id != DEVICE_ID) {
            continue;
        }
        {
            const struct app_mesh_discovery_assignment_snapshot snapshot = {
                .epoch = epoch,
                .local_id = DEVICE_ID,
                .gateway_id = GATEWAY_ID,
                .version = APP_MESH_DISCOVERY_ASSIGNMENT_SNAPSHOT_VERSION,
                .slot = entries[i].slot,
                .slot_count = slot_count,
                .valid = true,
            };

            ret = app_mesh_persistence_save_discovery_assignment(&snapshot);
            if (ret < 0) {
                LOG_WRN("anchor discovery assignment persistence failed: epoch=%u slot=%u ret=%d",
                        epoch,
                        entries[i].slot,
                        ret);
                return ret;
            }
        }
        ret = local_anchor_commit_discovery_assignment(epoch,
                                                       entries[i].slot,
                                                       slot_count);
        if (ret != PROTO_OK) {
            LOG_ERR("anchor discovery assignment commit rejected after persistence: epoch=%u slot=%u ret=%d",
                    epoch,
                    entries[i].slot,
                    ret);
            return -ESTALE;
        }
        ret = anchor_schedule_discovery_response(
            packet,
            epoch,
            DISCOVERY_ASSIGNMENT_PHASE_ACK,
            entries[i].slot,
            slot_count);
        if (ret < 0) {
            return ret;
        }
        status_debug_printf("DBG_DISCOVERY_SLOT_ASSIGNED state=PROVISIONED epoch=%u slot=%u slot_count=%u anchors=%u hash=0x%016llx\n",
                            epoch,
                            entries[i].slot,
                            slot_count,
                            (unsigned int)entry_count,
                            (unsigned long long)entries[i].hash);
        LOG_INF("anchor discovery slot assigned: epoch=%u slot=%u slot_count=%u anchors=%u",
                epoch,
                entries[i].slot,
                slot_count,
                (unsigned int)entry_count);
        return 0;
    }

    app_mesh_persistence_clear_discovery_assignment();
    local_anchor_mark_discovery_assignment_unprovisioned(epoch);
    status_debug_printf("DBG_DISCOVERY_SLOT_UNASSIGNED state=UNPROVISIONED epoch=%u count=%u reason=not-listed\n",
                        epoch,
                        (unsigned int)entry_count);
    LOG_WRN("anchor not listed in discovery-slot assignment: epoch=%u count=%u",
            epoch,
            (unsigned int)entry_count);
    return anchor_schedule_discovery_claim(packet, epoch);
}

static void anchor_execute_command_side_effects(const struct proto_packet *packet,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                enum command_id command_id,
                                                enum command_status *status,
                                                uint8_t *reason,
                                                bool *force_rediscovery_after_result,
                                                bool *reboot_after_result)
{
    enum device_role requested_role = ROLE_ANCHOR;
    int ret;

    if (command_id == CMD_REBOOT) {
        *reboot_after_result = true;
    } else if (command_id == CMD_SET_ROLE) {
        ret = gateway_command_extract_role(payload, payload_len, &requested_role);
        if (ret != PROTO_OK) {
            *status = COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        } else if ((uint8_t)requested_role != (uint8_t)DEVICE_ROLE) {
            *status = COMMAND_DENIED;
            *reason = 1u;
        }
    } else if (command_id == CMD_START_HEARTBEAT) {
        ret = anchor_start_heartbeat_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_STOP_HEARTBEAT) {
        anchor_stop_heartbeat();
    } else if (command_id == CMD_SET_LED_PATTERN) {
        ret = anchor_set_led_pattern_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_SET_ROUTE) {
        ret = anchor_set_route_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = ret == -ENOSPC ? COMMAND_BUSY : COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_CLEAR_ROUTE) {
        anchor_clear_route_from_command();
    } else if (command_id == CMD_FORCE_REDISCOVERY) {
        *force_rediscovery_after_result = true;
    } else if (command_id == CMD_SET_SCAN_DUTY) {
        ret = anchor_set_scan_duty_from_command(payload, payload_len, reason);
        if (ret < 0) {
            *status = COMMAND_MALFORMED_PAYLOAD;
        }
    } else if (command_id == CMD_ASSIGN_DISCOVERY_SLOTS) {
        ret = anchor_apply_discovery_assignment_command(packet,
                                                        payload,
                                                        payload_len);
        if (ret < 0) {
            *status = ret == -ESTALE ? COMMAND_INVALID_STATE :
                      COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_START_PAIR) {
        ret = app_anchor_survey_runtime_start_pair_from_command(packet,
                                                               payload,
                                                               payload_len,
                                                               status,
                                                               reason);
        if (ret < 0 && *status == COMMAND_OK) {
            *status = COMMAND_INTERNAL_ERROR;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id == CMD_SURVEY_ABORT) {
        struct survey_pair pair = {0};

        ret = survey_extract_pair_tlvs(payload, payload_len, &pair);
        if (ret == PROTO_OK) {
            (void)app_anchor_survey_runtime_abort_pair_matching(
                &pair,
                packet->session_id);
        } else if (ret == PROTO_ERR_NOT_FOUND && payload_len == 4u) {
            /* A command-ID-only host abort intentionally remains broad. */
            app_anchor_survey_runtime_abort_pair();
        } else {
            *status = COMMAND_MALFORMED_PAYLOAD;
            *reason = (uint8_t)(-ret);
        }
    } else if (command_id != CMD_PING && command_id != CMD_GET_STATUS) {
        *status = COMMAND_UNSUPPORTED_COMMAND;
        *reason = 1u;
    }
}

static void anchor_finish_broadcast_command(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            enum command_id command_id,
                                            const struct gateway_command_options *command_options)
{
    enum command_status status = COMMAND_OK;
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    uint8_t reason = 0u;
    int ret;

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    if (command_options->response_mode == CMD_RESPONSE_NONE) {
        LOG_INF("anchor broadcast command handled without result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        if (reboot_after_result && status == COMMAND_OK) {
            anchor_schedule_reboot_after_command_result();
        }
        return;
    }

    ret = anchor_schedule_collection_command_result(packet,
                                                    command_options,
                                                    command_id,
                                                    status,
                                                    reason,
                                                    force_rediscovery_after_result,
                                                    reboot_after_result);
    if (ret < 0) {
        LOG_WRN("anchor collection command result schedule failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
    } else {
        LOG_INF("anchor broadcast command handled with collection result: cmd=0x%04x command_seq=%u status=%u reason=%u",
                (unsigned int)command_id,
                command_options->command_seq,
                status,
                reason);
    }
}

static int anchor_schedule_broadcast_command_execution(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    const struct gateway_command_options *options,
    uint32_t delay_ms)
{
    if (packet == NULL || payload == NULL || options == NULL ||
        payload_len > sizeof(anchor_pending_command_execution.payload)) {
        return -EINVAL;
    }
    if (anchor_pending_command_execution.active) {
        return -EBUSY;
    }

    anchor_pending_command_execution.command = *packet;
    anchor_pending_command_execution.options.response_mode = options->response_mode;
    anchor_pending_command_execution.options.command_seq = options->command_seq;
    anchor_pending_command_execution.options.collection_epoch_id = options->collection_epoch_id;
    anchor_pending_command_execution.options.collection_slot_seed = options->collection_slot_seed;
    anchor_pending_command_execution.options.command_expiry_s = options->command_expiry_s;
    anchor_pending_command_execution.options.expected_node_count = options->expected_node_count;
    anchor_pending_command_execution.options.collection_required = options->collection_required;
    memcpy(anchor_pending_command_execution.payload, payload, payload_len);
    anchor_pending_command_execution.payload_len = (uint16_t)payload_len;
    anchor_pending_command_execution.received_at_ms = k_uptime_get_32();
    anchor_pending_command_execution.active = true;
    (void)k_work_reschedule(&anchor_command_execute_work,
                            K_MSEC(delay_ms == 0u ? 1u : delay_ms));
    LOG_INF("anchor broadcast command execution scheduled: command_seq=%u delay_ms=%u",
            options->command_seq,
            delay_ms);
    return 0;
}

static void anchor_command_execute_work_handler(struct k_work *work)
{
    struct anchor_pending_command_execution pending;
    struct gateway_command_options options = {0};
    enum command_id command_id = CMD_VENDOR_BASE;
    uint32_t now_ms;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR || !anchor_pending_command_execution.active) {
        return;
    }

    pending = anchor_pending_command_execution;
    memset(&anchor_pending_command_execution, 0, sizeof(anchor_pending_command_execution));

    now_ms = k_uptime_get_32();
    packet_age_add_elapsed(&pending.command, now_ms - pending.received_at_ms);
    anchor_pending_options_to_gateway(&pending.options, &options);

    if (gateway_command_receive_expired(&pending.command, &options)) {
        LOG_WRN("anchor dropped expired delayed broadcast command: command_seq=%u age_ms=%u expiry_s=%u",
                options.command_seq,
                pending.command.message_age_ms,
                options.command_expiry_s);
        return;
    }

    ret = gateway_command_extract_id(pending.payload, pending.payload_len, &command_id);
    if (ret != PROTO_OK) {
        LOG_WRN("anchor dropped malformed delayed broadcast command: command_seq=%u ret=%d",
                options.command_seq,
                ret);
        return;
    }

    anchor_finish_broadcast_command(&pending.command,
                                    pending.payload,
                                    pending.payload_len,
                                    command_id,
                                    &options);
}

static void anchor_handle_local_command(const struct proto_packet *packet,
                                        const uint8_t *payload,
                                        size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_OK;
    struct gateway_command_options command_options = {0};
    bool reboot_after_result = false;
    bool force_rediscovery_after_result = false;
    bool wait_for_delivery_completion;
    bool broadcast_command;
    bool expired;
    bool duplicate;
    uint32_t delivery_handle = 0u;
    uint32_t delay_ms;
    uint32_t now_ms;
    uint8_t completion_actions;
    uint8_t reason = 0u;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        packet == NULL ||
        packet->msg_type != MSG_COMMAND) {
        return;
    }
    broadcast_command = packet->dst_id == MESH_BROADCAST_ID;
    if (packet->dst_id != DEVICE_ID && !broadcast_command) {
        return;
    }

    now_ms = k_uptime_get_32();
    ret = app_mesh_command_orchestrator_anchor_receive(
        &anchor_command_orchestrator,
        packet,
        payload,
        payload_len,
        now_ms,
        &command_id,
        &command_options,
        &broadcast_command,
        &expired,
        &duplicate);
    if (ret != PROTO_OK) {
        if (broadcast_command) {
            LOG_WRN("anchor ignored invalid broadcast command: ret=%d", ret);
            return;
        }
        status = COMMAND_MALFORMED_PAYLOAD;
        reason = (uint8_t)(-ret);
    }

    if (broadcast_command) {
        if (expired) {
            LOG_WRN("anchor ignored expired broadcast command: cmd=0x%04x command_seq=%u age_ms=%u expiry_s=%u delay_ms=%u",
                    (unsigned int)command_id,
                    command_options.command_seq,
                    packet->message_age_ms,
                    command_options.command_expiry_s,
                    command_options.execute_delay_ms);
            return;
        }
        if (duplicate) {
            LOG_INF("anchor ignored duplicate broadcast command: cmd=0x%04x command_seq=%u",
                    (unsigned int)command_id,
                    command_options.command_seq);
            return;
        }

        delay_ms = gateway_command_execute_delay_remaining_ms(packet, &command_options);
        if (delay_ms != 0u) {
            ret = anchor_schedule_broadcast_command_execution(packet,
                                                              payload,
                                                              payload_len,
                                                              &command_options,
                                                              delay_ms);
            if (ret < 0) {
                LOG_WRN("anchor delayed broadcast command schedule failed: cmd=0x%04x command_seq=%u ret=%d",
                        (unsigned int)command_id,
                        command_options.command_seq,
                        ret);
                return;
            }
            return;
        }

        anchor_finish_broadcast_command(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &command_options);
        return;
    }

    anchor_execute_command_side_effects(packet,
                                        payload,
                                        payload_len,
                                        command_id,
                                        &status,
                                        &reason,
                                        &force_rediscovery_after_result,
                                        &reboot_after_result);

    completion_actions = anchor_command_completion_actions(
        status, force_rediscovery_after_result, reboot_after_result);
    wait_for_delivery_completion =
        (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) ||
        completion_actions != 0u;
    ret = anchor_submit_command_result(packet,
                                       command_id,
                                       status,
                                       reason,
                                       NULL,
                                       0u,
                                       wait_for_delivery_completion ?
                                           &delivery_handle : NULL);
    if (ret < 0) {
        LOG_WRN("anchor command result TX failed: cmd=0x%04x status=%u ret=%d",
                (unsigned int)command_id,
                status,
                ret);
        if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
            (void)app_anchor_survey_runtime_cancel_pair_start(packet);
        }
        if (force_rediscovery_after_result && status == COMMAND_OK) {
            anchor_force_rediscovery_from_command();
        }
        return;
    }
    if (command_id == CMD_SURVEY_START_PAIR && status == COMMAND_OK) {
        int bind_ret = app_anchor_survey_runtime_bind_pair_start_delivery(
            packet, delivery_handle);

        if (bind_ret < 0 && bind_ret != -ESTALE) {
            LOG_WRN("survey pair start delivery binding failed: ret=%d",
                    bind_ret);
        }
        if (bind_ret < 0) {
            (void)app_node_comm_auto_reap_delivery(delivery_handle);
        }
    }
    if (completion_actions != 0u) {
        ret = app_anchor_command_completion_watch(delivery_handle,
                                                  command_id,
                                                  completion_actions);
        if (ret < 0) {
            (void)app_node_comm_abandon_delivery(delivery_handle);
            LOG_ERR("anchor command completion admission failed: cmd=0x%04x ret=%d",
                    (unsigned int)command_id, ret);
            return;
        }
    }

    LOG_INF("anchor command handled: cmd=0x%04x status=%u reason=%u",
            (unsigned int)command_id,
            status,
            reason);
}

static bool gateway_command_uses_survey_mesh(enum command_id command_id)
{
    return command_id == CMD_SURVEY_REACHABILITY ||
           command_id == CMD_SURVEY_PREPARE_PAIR ||
           command_id == CMD_SURVEY_START_PAIR;
}

static int gateway_extract_survey_sample_count(const uint8_t *payload,
                                                size_t payload_len,
                                                uint16_t *sample_count)
{
    uint16_t value = SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT;
    int ret;

    if (sample_count == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = command_find_u16_tlv(payload, payload_len, TLV_SAMPLE_COUNT, &value);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    if (!survey_sample_count_valid(value)) {
        return PROTO_ERR_MALFORMED;
    }

    *sample_count = value;
    return PROTO_OK;
}

static enum gateway_command_event_kind gateway_observability_kind(
    enum command_id command_id)
{
    switch (command_id) {
    case CMD_ASSIGN_DISCOVERY_SLOTS:
        return GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION;
    case CMD_SURVEY_REACHABILITY:
    case CMD_SURVEY_PREPARE_PAIR:
    case CMD_SURVEY_START_PAIR:
    case CMD_SURVEY_ABORT:
        return GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY;
    case CMD_FORCE_REDISCOVERY:
        return GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH;
    default:
        return 0;
    }
}

static struct gateway_command_event gateway_observability_event(
    enum gateway_command_event_kind kind,
    enum gateway_command_event_stage stage,
    enum command_id command_id,
    const struct proto_packet *host_command,
    uint32_t gateway_sequence)
{
    struct gateway_command_event event = {
        .kind = kind,
        .stage = stage,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = command_id,
        .gateway_epoch = (uint16_t)mesh_runtime.upstream.current_epoch,
        .gateway_sequence = gateway_sequence,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };

    if (host_command != NULL) {
        event.host_session_id = host_command->session_id;
        event.host_seq = host_command->seq;
        event.correlation_id = host_command->session_id != 0u ?
                               host_command->session_id : host_command->seq;
    }
    return event;
}

static void gateway_route_refresh_observe(
    const struct app_node_comm_route_refresh_event *refresh)
{
    struct gateway_command_event event;
    enum gateway_command_event_stage stage;

    if (refresh == NULL || !refresh->correlated ||
        DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
    switch (refresh->kind) {
    case APP_NODE_COMM_ROUTE_REFRESH_FLOOD_ATTEMPT:
        stage = GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT;
        break;
    case APP_NODE_COMM_ROUTE_REFRESH_BACKOFF:
        stage = GATEWAY_COMMAND_EVENT_STAGE_BACKOFF;
        break;
    case APP_NODE_COMM_ROUTE_REFRESH_COMPLETE:
        stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE;
        break;
    default:
        return;
    }

    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ROUTE_REFRESH,
        stage,
        CMD_FORCE_REDISCOVERY,
        &refresh->correlation,
        refresh->gateway_sequence);
    event.attempt = refresh->attempt;
    event.progress_count = refresh->sent_count;

    if (refresh->kind == APP_NODE_COMM_ROUTE_REFRESH_COMPLETE) {
        if (refresh->result == 0) {
            event.status = COMMAND_OK;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
        } else if (refresh->result == -ETIMEDOUT) {
            event.status = COMMAND_TIMEOUT;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_TIMEOUT;
        } else if (refresh->result == -EIO ||
                   refresh->result == -ECANCELED) {
            event.status = COMMAND_RADIO_ERROR;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
        } else {
            event.status = COMMAND_INTERNAL_ERROR;
            event.reason = GATEWAY_COMMAND_EVENT_REASON_INTERNAL;
        }
        gateway_emit_host_command_result(&refresh->correlation,
                                         CMD_FORCE_REDISCOVERY,
                                         event.status,
                                         (uint8_t)event.reason);
        (void)gateway_observe_command_event(&event, true);
        return;
    }
    if (refresh->kind == APP_NODE_COMM_ROUTE_REFRESH_BACKOFF) {
        event.reason = GATEWAY_COMMAND_EVENT_REASON_RADIO;
    }
    (void)gateway_observe_command_event(&event, false);
}

#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observe_host_stage(const struct proto_packet *host_command,
                                       enum command_id command_id,
                                       enum gateway_command_event_stage stage)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event event;

    if (kind == 0 || host_command == NULL) {
        return;
    }
    event = gateway_observability_event(kind,
                                        stage,
                                        command_id,
                                        host_command,
                                        0u);
    (void)gateway_observe_command_event(&event, false);
}

static int gateway_observe_host_acceptance(
    const struct proto_packet *host_command,
    enum command_id command_id)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event queued;

    if (kind == 0 || host_command == NULL) {
        return 0;
    }
    queued = gateway_observability_event(
        kind, GATEWAY_COMMAND_EVENT_STAGE_QUEUED, command_id,
        host_command, 0u);
    return gateway_observe_command_acceptance_if_available(&queued);
}
#endif

static void gateway_observe_host_terminal(
    const struct proto_packet *host_command,
    enum command_id command_id,
    enum command_status status,
    enum gateway_command_event_reason reason)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);
    struct gateway_command_event event;

    if (kind == 0 || host_command == NULL) {
        return;
    }
    event = gateway_observability_event(kind,
                                        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
                                        command_id,
                                        host_command,
                                        0u);
    event.status = status;
    event.reason = reason;
    (void)gateway_observe_command_event(&event, true);
}

static void gateway_observe_survey_terminal(
    enum command_status status,
    enum gateway_command_event_reason reason)
{
    struct gateway_command_event event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        CMD_SURVEY_REACHABILITY,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);

    event.status = status;
    event.reason = reason;
    event.progress_count = (uint16_t)gateway_survey_context.next_pair_index;
    event.total_count = (uint16_t)gateway_survey_context.pair_count;
    event.success_count = gateway_survey_pair_success_count;
    event.failure_count = gateway_survey_pair_failure_count;
    event.duplicate_count = gateway_survey_duplicate_count;
    (void)gateway_observe_command_event(&event, true);
}

static int gateway_survey_observe_with_custody(
    struct gateway_command_event *event,
    bool terminal)
{
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
    return gateway_observe_command_event_if_available(event, terminal, NULL);
#else
    return gateway_observe_command_event(event, terminal);
#endif
}

static int gateway_survey_observe_callback(
    struct gateway_command_event *event,
    bool terminal,
    void *ctx)
{
    ARG_UNUSED(ctx);
    return gateway_survey_observe_with_custody(event, terminal);
}

static const struct app_gateway_survey_observability_ops
    gateway_survey_observability_ops = {
        .emit_if_available = gateway_survey_observe_callback,
        .ctx = NULL,
    };

static bool gateway_survey_flush_boundary_event(void)
{
    int ret = app_gateway_survey_observability_flush_boundary(
        &gateway_survey_observability, &gateway_survey_observability_ops);

    if (ret < 0) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_BLE_TX_RETRY_MS));
        return false;
    }
    return true;
}

static bool gateway_survey_emit_collection_telemetry(void)
{
    struct gateway_command_event base = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
        CMD_SURVEY_REACHABILITY,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);
    int ret = app_gateway_survey_observability_emit_collection_next(
        &gateway_survey_observability, &gateway_survey_observability_ops,
        &gateway_survey_context, &base, gateway_survey_duplicate_count);

    if (ret <= 0) {
        (void)k_work_reschedule(
            &gateway_survey_work,
            ret < 0 ? K_MSEC(GATEWAY_BLE_TX_RETRY_MS) : K_NO_WAIT);
        return false;
    }
    return true;
}

static int gateway_reject_survey_request(
    const struct proto_packet *host_packet,
    enum command_status status,
    uint8_t protocol_reason,
    enum gateway_command_event_reason event_reason,
    int error)
{
    gateway_emit_host_command_result(host_packet,
                                     CMD_SURVEY_REACHABILITY,
                                     status,
                                     protocol_reason);
    if (host_packet != NULL) {
        gateway_observe_host_terminal(host_packet,
                                      CMD_SURVEY_REACHABILITY,
                                      status,
                                      event_reason);
    }
    return error;
}

static int gateway_route_survey_reachability(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len)
{
    const struct survey_discovery_config discovery_config = {
        .survey_id = 0u,
        .start_delay_ms = SURVEY_DISCOVERY_START_DELAY_MS,
        .slot_ms = SURVEY_DISCOVERY_SLOT_MS,
        .slot_count = SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
    };
    struct survey_discovery_config config = discovery_config;
    struct mesh_outbound outbound = {0};
    uint32_t survey_id = 0u;
    uint32_t duration_ms = 0u;
    uint32_t discovery_duration_ms = 0u;
    uint32_t report_mesh_duration_ms = 0u;
    uint32_t collection_delay_ms = 0u;
    uint32_t command_budget_ms = GATEWAY_COMMAND_BUDGET_MAX_MS;
    uint32_t command_origin_ms;
    uint16_t sample_count = UWB_SAMPLES_PER_ANCHOR;
    bool budget_explicit = false;
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (host_packet->dst_id != MESH_BROADCAST_ID && host_packet->dst_id != DEVICE_ID)) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_DENIED, 1u,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
    }
    if (gateway_survey_active
#if DEVICE_ROLE == ROLE_GATEWAY
        || gateway_survey_cleanup_pending()
#endif
    ) {
        LOG_WRN("gateway survey reachability rejected while survey active or cleaning up: current=%u requested_dst=0x%016llx",
                gateway_survey_context.survey_id,
                (unsigned long long)host_packet->dst_id);
        return gateway_reject_survey_request(
            host_packet, COMMAND_BUSY, 3u,
            GATEWAY_COMMAND_EVENT_REASON_BUSY, -EBUSY);
    }

    ret = survey_extract_reach_request_tlvs(host_payload,
                                            host_payload_len,
                                            &survey_id,
                                            &duration_ms);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    ret = gateway_command_extract_budget_ms(host_payload,
                                            host_payload_len,
                                            GATEWAY_COMMAND_BUDGET_MAX_MS,
                                            &command_budget_ms,
                                            &budget_explicit);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }

    ret = gateway_extract_survey_sample_count(host_payload,
                                              host_payload_len,
                                              &sample_count);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    {
        enum command_status admission_status;
        enum gateway_command_event_reason admission_reason;

        if (!gateway_command_survey_sample_admission(sample_count,
                                                     &admission_status,
                                                     &admission_reason)) {
        LOG_WRN("gateway survey reachability rejected: samples=%u runtime_max=%u",
                sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
            return gateway_reject_survey_request(host_packet,
                                                 admission_status,
                                                 4u,
                                                 admission_reason,
                                                 -EINVAL);
        }
    }
    ret = survey_extract_discovery_slot_count_tlv(host_payload,
                                                  host_payload_len,
                                                  SURVEY_DISCOVERY_DEFAULT_SLOT_COUNT,
                                                  &config.slot_count);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    config.survey_id = survey_id;
    discovery_duration_ms = survey_discovery_duration_ms(&config);
    ret = survey_discovery_report_delay_ms(&config,
                                           config.slot_count - 1u,
                                           SURVEY_RESULT_MESH_SLOT_MS,
                                           &report_mesh_duration_ms);
    if (ret == PROTO_OK && UINT32_MAX - report_mesh_duration_ms >=
        SURVEY_RESULT_MESH_SLOT_MS) {
        report_mesh_duration_ms += SURVEY_RESULT_MESH_SLOT_MS;
    } else {
        ret = PROTO_ERR_NO_SPACE;
    }
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST,
            mesh_errno_from_proto(ret));
    }
    if (discovery_duration_ms == 0u ||
        UINT32_MAX - config.start_delay_ms < report_mesh_duration_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms < duration_ms ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms - duration_ms <
            SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS ||
        UINT32_MAX - config.start_delay_ms - report_mesh_duration_ms - duration_ms -
            SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS <
            SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_MALFORMED_PAYLOAD, 2u,
            GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST, -EINVAL);
    }
    collection_delay_ms = config.start_delay_ms + report_mesh_duration_ms +
                          duration_ms +
                          SURVEY_DISCOVERY_REPORT_CUSTODY_TIMEOUT_MS +
                          SURVEY_DISCOVERY_REPORT_DELIVERY_TAIL_MS;
    if (budget_explicit) {
        collection_delay_ms = gateway_command_budget_window_ms(
            true, command_budget_ms, 3u, collection_delay_ms);
    }

    ret = survey_append_discovery_start_tlvs(outbound.payload,
                                             sizeof(outbound.payload),
                                             &payload_len,
                                             &config);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    ret = survey_init_discovery_start_packet(&outbound.packet,
                                             DEVICE_ID,
                                             &config,
                                             seq,
                                             (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = MESH_BROADCAST_ID;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    command_origin_ms = k_uptime_get_32();
    outbound.queued_at_ms = command_origin_ms == 0u ? 1u : command_origin_ms;

    ret = survey_gateway_begin(&gateway_survey_context, survey_id, sample_count);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
    ret = survey_gateway_auto_begin(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        return gateway_reject_survey_request(
            host_packet, COMMAND_INTERNAL_ERROR, (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            mesh_errno_from_proto(ret));
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    survey_gateway_transaction_init(&gateway_survey_transaction);
    memset(&gateway_survey_cleanup, 0, sizeof(gateway_survey_cleanup));
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
#endif
    gateway_survey_active = true;
    gateway_survey_operation_deadline_ms = command_origin_ms + command_budget_ms;
    gateway_survey_budget_explicit = budget_explicit;
    gateway_survey_host_command = *host_packet;
    gateway_survey_duplicate_count = 0u;
    gateway_survey_pair_success_count = 0u;
    gateway_survey_pair_failure_count = 0u;
    gateway_survey_terminal_failure_reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
    gateway_survey_pair_result_mask = 0u;
    gateway_survey_pair_range_failure_count = 0u;
    gateway_survey_pair_observation_active = false;
    app_gateway_survey_observability_reset(&gateway_survey_observability);
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING,
            CMD_SURVEY_REACHABILITY,
            host_packet,
            survey_id);

        event.total_count = config.slot_count;
        (void)gateway_observe_command_event(&event, false);
    }

    ret = app_node_comm_send_control_flood(
        &outbound,
        C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
        "survey-discovery-start",
        NULL);
    if (ret < 0) {
        gateway_survey_active = false;
        (void)survey_gateway_auto_begin(&gateway_survey_auto);
        (void)k_work_cancel_delayable(&gateway_survey_work);
        return gateway_reject_survey_request(
            host_packet,
            ret == -EBUSY ? COMMAND_BUSY : COMMAND_RADIO_ERROR,
            (uint8_t)(-ret),
            ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                            GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION,
            ret);
    }

    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
            GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT,
            CMD_SURVEY_REACHABILITY,
            host_packet,
            survey_id);

        event.attempt = 1u;
        event.total_count = config.slot_count;
        (void)gateway_observe_command_event(&event, false);
    }

    (void)k_work_reschedule(
        &gateway_survey_work,
        K_MSEC(uptime_ms_until_deadline(
            k_uptime_get_32(), command_origin_ms + collection_delay_ms)));
    gateway_emit_host_command_result(host_packet, CMD_SURVEY_REACHABILITY, COMMAND_OK, 0u);
    LOG_INF("gateway survey discovery broadcast: survey=%u start_delay_ms=%u slot_ms=%u slots=%u discovery_ms=%u report_train_end_ms=%u report_grace_ms=%u samples=%u seq=%u",
            survey_id,
            config.start_delay_ms,
            config.slot_ms,
            config.slot_count,
            discovery_duration_ms,
            report_mesh_duration_ms,
            duration_ms,
            sample_count,
            seq);
    return 0;
}

static void gateway_note_survey_pair_result(const struct proto_packet *packet,
                                            const uint8_t *payload,
                                            size_t payload_len)
{
    uint64_t initiator_id = 0u;
    uint64_t responder_id = 0u;
    uint32_t survey_id = 0u;
    uint16_t sample_index = 0u;
    uint8_t range_status = RANGE_INTERNAL_ERROR;
    uint16_t sample_bit;

    if (!gateway_survey_pair_observation_active || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_SURVEY_PAIR_RESULT ||
        command_find_u32_tlv(payload, payload_len, TLV_SURVEY_ID, &survey_id) != PROTO_OK ||
        command_find_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, &initiator_id) != PROTO_OK ||
        command_find_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, &responder_id) != PROTO_OK ||
        command_find_u16_tlv(payload, payload_len, TLV_SAMPLE_INDEX, &sample_index) != PROTO_OK ||
        command_find_u8_tlv(payload, payload_len, TLV_RANGE_STATUS, &range_status) != PROTO_OK ||
        survey_id != gateway_survey_auto.pair.survey_id ||
        initiator_id != gateway_survey_auto.pair.initiator_id ||
        responder_id != gateway_survey_auto.pair.responder_id ||
        packet->src_id != gateway_survey_auto.pair.initiator_id ||
        sample_index >= gateway_survey_auto.pair.sample_count || sample_index >= 16u) {
        return;
    }

    sample_bit = (uint16_t)(UINT16_C(1) << sample_index);
    if ((gateway_survey_pair_result_mask & sample_bit) != 0u) {
        if (gateway_survey_duplicate_count < UINT16_MAX) {
            gateway_survey_duplicate_count++;
        }
        return;
    }
    gateway_survey_pair_result_mask |= sample_bit;
    if (range_status != RANGE_OK && gateway_survey_pair_range_failure_count < UINT16_MAX) {
        gateway_survey_pair_range_failure_count++;
    }
    if ((uint16_t)__builtin_popcount(gateway_survey_pair_result_mask) ==
        gateway_survey_auto.pair.sample_count) {
        (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
    }
}

static void gateway_handle_survey_discovery_report(const struct proto_packet *packet,
                                                    const uint8_t *payload,
                                                    size_t payload_len,
                                                    uint64_t previous_hop_id,
                                                    uint8_t radio_channel,
                                                    uint8_t link_quality)
{
    struct survey_reachability_entry entries[SURVEY_GATEWAY_MAX_PEERS_PER_REPORT];
    struct survey_gateway_reverse_hint reverse_hint;
    uint32_t survey_id = 0u;
    uint64_t anchor_id = 0u;
    size_t entry_count = 0u;
    bool duplicate_report = false;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL || payload == NULL) {
        return;
    }
    if (packet->msg_type == MSG_SURVEY_PAIR_RESULT) {
        gateway_note_survey_pair_result(packet, payload, payload_len);
        return;
    }
    if (packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT ||
        packet->dst_id != DEVICE_ID) {
        return;
    }
    if (radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        !mesh_id_is_unicast(previous_hop_id) ||
        link_quality > 100u) {
        LOG_WRN("gateway survey discovery report rejected transport: src=0x%016llx prev=0x%016llx channel=%u flags=0x%02x quality=%u",
                (unsigned long long)packet->src_id,
                (unsigned long long)previous_hop_id,
                radio_channel,
                packet->flags,
                link_quality);
        return;
    }

    ret = survey_extract_reach_report_tlvs(payload,
                                           payload_len,
                                           &survey_id,
                                           &anchor_id,
                                           entries,
                                           ARRAY_SIZE(entries),
                                           &entry_count);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report rejected: ret=%d src=0x%016llx session=%u",
                ret,
                (unsigned long long)packet->src_id,
                packet->session_id);
        return;
    }
    if (packet->session_id != survey_id || packet->src_id != anchor_id) {
        LOG_WRN("gateway survey discovery report identity mismatch: pkt_session=%u survey=%u src=0x%016llx anchor=0x%016llx",
                packet->session_id,
                survey_id,
                (unsigned long long)packet->src_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (!gateway_survey_active ||
        gateway_survey_context.survey_id != survey_id) {
        LOG_WRN("gateway survey discovery report stale: survey=%u active=%u current=%u anchor=0x%016llx",
                survey_id,
                gateway_survey_active ? 1u : 0u,
                gateway_survey_context.survey_id,
                (unsigned long long)anchor_id);
        return;
    }
    if (gateway_survey_auto.running) {
        LOG_WRN("gateway survey discovery report ignored after orchestration start: survey=%u anchor=0x%016llx entries=%u",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count);
        return;
    }

    for (size_t i = 0u; i < gateway_survey_context.report_count; i++) {
        if (gateway_survey_context.reports[i].valid &&
            gateway_survey_context.reports[i].anchor_id == anchor_id) {
            if (gateway_survey_duplicate_count < UINT16_MAX) {
                gateway_survey_duplicate_count++;
            }
            duplicate_report = true;
            break;
        }
    }
    if (duplicate_report) {
        LOG_INF("gateway survey duplicate report ignored: survey=%u anchor=0x%016llx entries=%u prev=0x%016llx",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                (unsigned long long)previous_hop_id);
        return;
    }

    reverse_hint = (struct survey_gateway_reverse_hint) {
        .target_id = anchor_id,
        .next_hop_id = previous_hop_id,
        .quality = link_quality,
        .valid = true,
    };
    ret = survey_gateway_note_reach_report_with_reverse_hint(
        &gateway_survey_context,
        survey_id,
        anchor_id,
        entries,
        entry_count,
        &reverse_hint);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey discovery report not recorded: survey=%u anchor=0x%016llx entries=%u ret=%d",
                survey_id,
                (unsigned long long)anchor_id,
                (unsigned int)entry_count,
                ret);
        return;
    }
    app_stack_workload_diag_gateway_report_cycle(
        packet, (uint16_t)gateway_survey_context.report_count, 1u);

    ret = survey_gateway_plan_pairs(&gateway_survey_context);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey pair planning failed: survey=%u reports=%u ret=%d",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                ret);
        return;
    }

    if (gateway_survey_context.pair_count > 0u) {
        struct survey_pair pair;

        ret = survey_gateway_pair_at(&gateway_survey_context, 0u, &pair);
        if (ret != PROTO_OK) {
            LOG_WRN("gateway survey first pair unavailable: survey=%u ret=%d",
                    survey_id,
                    ret);
            return;
        }

        LOG_INF("gateway survey report recorded: survey=%u reports=%u pairs=%u first=0x%016llx->0x%016llx samples=%u",
                survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count,
                (unsigned long long)pair.initiator_id,
                (unsigned long long)pair.responder_id,
                pair.sample_count);
    } else {
        LOG_INF("gateway survey report recorded: survey=%u reports=%u pairs=0",
                survey_id,
                (unsigned int)gateway_survey_context.report_count);
    }
}

static uint32_t gateway_survey_pair_run_delay_ms(const struct survey_pair *pair)
{
    uint32_t per_sample_ms = SURVEY_PAIR_RESPONDER_WINDOW_MS + SURVEY_PAIR_SAMPLE_GAP_MS;

    if (pair == NULL || pair->sample_count == 0u) {
        return GATEWAY_SURVEY_AUTO_RETRY_MS;
    }
    if (pair->sample_count > (UINT32_MAX - GATEWAY_SURVEY_PAIR_SETTLE_MS) / per_sample_ms) {
        return UINT32_MAX;
    }
    return ((uint32_t)pair->sample_count * per_sample_ms) + GATEWAY_SURVEY_PAIR_SETTLE_MS;
}

static bool gateway_survey_finalize_pair_observation(void)
{
    struct gateway_command_event event;
    uint16_t observed_count;
    bool success;

    if (!gateway_survey_pair_observation_active) {
        return true;
    }
    observed_count = (uint16_t)__builtin_popcount(
        gateway_survey_pair_result_mask);
    success = observed_count == gateway_survey_auto.pair.sample_count &&
              gateway_survey_pair_range_failure_count == 0u;
    event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        success ? GATEWAY_COMMAND_EVENT_STAGE_PAIR_SUCCESS :
                  GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        CMD_SURVEY_START_PAIR,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);
    event.status = success ? COMMAND_OK : COMMAND_TIMEOUT;
    event.reason = success ? GATEWAY_COMMAND_EVENT_REASON_NONE :
                   gateway_survey_pair_range_failure_count > 0u ?
                   GATEWAY_COMMAND_EVENT_REASON_PAIR_RANGE_FAILED :
                   GATEWAY_COMMAND_EVENT_REASON_PAIR_INCOMPLETE;
    event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
    event.pair_responder_id = gateway_survey_auto.pair.responder_id;
    event.progress_count = observed_count;
    event.total_count = gateway_survey_auto.pair.sample_count;
    event.success_count = (uint16_t)(gateway_survey_pair_success_count +
                                    (success ? 1u : 0u));
    event.failure_count = (uint16_t)(gateway_survey_pair_failure_count +
                                    (success ? 0u : 1u));
    event.duplicate_count = gateway_survey_duplicate_count;
    if (gateway_survey_observe_with_custody(&event, false) < 0) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_BLE_TX_RETRY_MS));
        return false;
    }
    if (success) {
        gateway_survey_pair_success_count++;
    } else {
        gateway_survey_pair_failure_count++;
        gateway_survey_terminal_failure_reason =
            gateway_command_survey_failure_reason_merge(
                gateway_survey_terminal_failure_reason,
                event.reason);
    }
    gateway_survey_pair_observation_active = false;
    gateway_survey_pair_result_mask = 0u;
    gateway_survey_pair_range_failure_count = 0u;
#if DEVICE_ROLE == ROLE_GATEWAY
    survey_gateway_transaction_pair_complete(&gateway_survey_transaction,
                                             success,
                                             (uint64_t)k_uptime_get());
    if (!success) {
        gateway_survey_begin_cleanup();
        if (survey_gateway_auto_no_unstarted_pairs(
                &gateway_survey_auto, &gateway_survey_context)) {
            gateway_survey_auto_finish();
        }
        return false;
    }
#endif
    return true;
}

static void gateway_survey_auto_finish_status(
    enum command_status status,
    enum gateway_command_event_reason reason)
{
    LOG_INF("gateway survey orchestration complete: survey=%u planned_pairs=%u",
            gateway_survey_context.survey_id,
            (unsigned int)gateway_survey_context.pair_count);
    if (!gateway_survey_finalize_pair_observation()) {
        return;
    }
    gateway_observe_survey_terminal(status, reason);
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_transaction.active.state != NODE_TRANSACTION_EMPTY &&
        !gateway_survey_transaction.active.request_delivery_terminal) {
        enum node_transaction_action action;
        int ret = gateway_survey_cancel_take_active_delivery(&action);

        if (ret < 0) {
            LOG_ERR("gateway survey finish delivery cancel/take failed: ret=%d",
                    ret);
        }
    }
    survey_gateway_transaction_require_cleanup(
        &gateway_survey_transaction, false, (uint64_t)k_uptime_get());
    gateway_survey_begin_cleanup();
#endif
    gateway_survey_active = false;
    gateway_survey_budget_explicit = false;
    gateway_survey_operation_deadline_ms = 0u;
    if (gateway_survey_pending_command_valid) {
        app_stack_workload_diag_gateway_control_release(
            &gateway_survey_pending_command, -ECANCELED, 0u, 0u);
        gateway_clear_pending_command_result(&gateway_survey_pending_command);
        gateway_survey_pending_command_valid = false;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_cleanup_pending()) {
        (void)k_work_reschedule(
            &gateway_survey_work,
            K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
    } else {
        (void)k_work_cancel_delayable(&gateway_survey_work);
    }
#else
    (void)k_work_cancel_delayable(&gateway_survey_work);
#endif
    (void)survey_gateway_auto_begin(&gateway_survey_auto);
}

static void gateway_survey_auto_finish(void)
{
    enum command_status status;
    enum gateway_command_event_reason reason;

    gateway_command_survey_terminal_outcome(
        gateway_survey_context.report_count,
        gateway_survey_pair_failure_count,
        gateway_survey_terminal_failure_reason,
        &status,
        &reason);
    gateway_survey_auto_finish_status(status, reason);
}

static int gateway_survey_prepare_pair_control(struct mesh_outbound *outbound)
{
    struct survey_gateway_reverse_hint reverse_hint;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = survey_gateway_reverse_hint_for_target(&gateway_survey_context,
                                                 outbound->packet.dst_id,
                                                 &reverse_hint);
    if (ret == PROTO_OK) {
        ret = mesh_relay_note_gateway_survey_reverse_route(
            &mesh_runtime,
            reverse_hint.target_id,
            reverse_hint.next_hop_id,
            reverse_hint.quality,
            k_uptime_get_32());
        if (ret != PROTO_OK) {
            LOG_WRN("gateway survey reverse route install failed: dst=0x%016llx next=0x%016llx ret=%d",
                    (unsigned long long)reverse_hint.target_id,
                    (unsigned long long)reverse_hint.next_hop_id,
                    ret);
            return mesh_errno_from_proto(ret);
        }
        outbound->next_hop_id = reverse_hint.next_hop_id;
    } else if (ret == PROTO_ERR_NOT_FOUND) {
        return -EHOSTUNREACH;
    } else {
        return mesh_errno_from_proto(ret);
    }
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    return 0;
}

static int gateway_survey_send_pair_control(struct mesh_outbound *outbound,
                                            uint64_t absolute_deadline_ms,
                                            uint32_t client_token,
                                            uint32_t *delivery_handle_out)
{
    int ret;

    if (absolute_deadline_ms == 0u || delivery_handle_out == NULL) {
        return -EINVAL;
    }
    ret = gateway_survey_prepare_pair_control(outbound);
    if (ret < 0) {
        return ret;
    }

    return app_node_comm_submit_delivery(
        outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        absolute_deadline_ms,
        client_token,
        delivery_handle_out);
}

#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_survey_next_transaction_token(void)
{
    gateway_survey_transaction_client_token++;
    if (gateway_survey_transaction_client_token == 0u) {
        gateway_survey_transaction_client_token = 1u;
    }
    return gateway_survey_transaction_client_token;
}

static uint8_t gateway_survey_remaining_control_phases(void)
{
    size_t future_pairs = 0u;
    size_t phases;

    if (gateway_survey_context.pair_count >
        gateway_survey_context.next_pair_index) {
        future_pairs = gateway_survey_context.pair_count -
                       gateway_survey_context.next_pair_index;
    }
    phases = future_pairs * 4u;
    switch (gateway_survey_auto.stage) {
    case SURVEY_GATEWAY_AUTO_PREPARE_INITIATOR:
        phases += 4u;
        break;
    case SURVEY_GATEWAY_AUTO_PREPARE_RESPONDER:
        phases += 3u;
        break;
    case SURVEY_GATEWAY_AUTO_START_RESPONDER:
        phases += 2u;
        break;
    case SURVEY_GATEWAY_AUTO_START_INITIATOR:
        phases += 1u;
        break;
    case SURVEY_GATEWAY_AUTO_IDLE:
    case SURVEY_GATEWAY_AUTO_LOAD_PAIR:
    default:
        phases += 1u;
        break;
    }
    return phases > UINT8_MAX ? UINT8_MAX : (uint8_t)phases;
}

static uint32_t gateway_survey_control_timeout_ms(void)
{
    uint32_t remaining_ms;

    if (!gateway_survey_budget_explicit) {
        return SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
    }
    remaining_ms = uptime_ms_until_deadline(
        k_uptime_get_32(), gateway_survey_operation_deadline_ms);
    return gateway_command_budget_window_ms(
        true,
        remaining_ms,
        gateway_survey_remaining_control_phases(),
        SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
}

static int gateway_survey_outbound_fingerprint(
    const struct mesh_outbound *outbound,
    uint32_t *fingerprint)
{
    uint8_t encoded[UWB_MESH_MAX_FRAME_LEN];
    size_t encoded_len = 0u;
    int ret;

    if (outbound == NULL || fingerprint == NULL ||
        outbound->packet.payload_len != outbound->payload_len) {
        return -EINVAL;
    }
    ret = proto_packet_encode(&outbound->packet,
                              outbound->payload,
                              encoded,
                              sizeof(encoded),
                              &encoded_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    *fingerprint = node_transaction_fingerprint_bytes(0u,
                                                       encoded,
                                                       encoded_len);
    return *fingerprint == 0u ? -EINVAL : 0;
}

static bool gateway_survey_pair_matches_transaction(
    const struct survey_pair *pair)
{
    return pair != NULL && gateway_survey_transaction.pair_loaded &&
           gateway_survey_transaction.pair.survey_id == pair->survey_id &&
           gateway_survey_transaction.pair.initiator_id == pair->initiator_id &&
           gateway_survey_transaction.pair.responder_id == pair->responder_id &&
           gateway_survey_transaction.pair.sample_count == pair->sample_count;
}

static int gateway_survey_transaction_load_action_pair(
    const struct survey_pair *pair)
{
    if (gateway_survey_pair_matches_transaction(pair)) {
        return 0;
    }
    if (gateway_survey_transaction.pair_loaded) {
        return -ESTALE;
    }
    return survey_gateway_transaction_load_pair(&gateway_survey_transaction,
                                                pair);
}
#endif

static int gateway_survey_auto_send_outbound(struct mesh_outbound *outbound,
                                             enum command_id command_id,
                                             const char *reason)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct node_transaction_key key;
    uint64_t now_ms;
    uint64_t absolute_deadline_ms;
    uint32_t request_fingerprint = 0u;
    uint32_t control_timeout_ms;
    uint32_t client_token;
    uint32_t delivery_handle;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = gateway_survey_outbound_fingerprint(outbound,
                                              &request_fingerprint);
    if (ret < 0) {
        return ret;
    }
    now_ms = (uint64_t)k_uptime_get();
    control_timeout_ms = gateway_survey_control_timeout_ms();
    if (control_timeout_ms == 0u) {
        return -ETIMEDOUT;
    }
    absolute_deadline_ms = now_ms + control_timeout_ms;
    client_token = gateway_survey_next_transaction_token();
    ret = gateway_survey_send_pair_control(
        outbound,
        absolute_deadline_ms,
        client_token,
        &delivery_handle);
    if (ret < 0) {
        return ret;
    }

    key = (struct node_transaction_key) {
        .requester_id = outbound->packet.src_id,
        .responder_id = outbound->packet.dst_id,
        .session_id = outbound->packet.session_id,
        .transaction_id = outbound->packet.seq,
        .operation_id = (uint16_t)command_id,
    };
    ret = survey_gateway_transaction_begin(&gateway_survey_transaction,
                                           &key,
                                           command_id,
                                           request_fingerprint,
                                           client_token,
                                           delivery_handle,
                                           absolute_deadline_ms,
                                           now_ms);
    if (ret < 0) {
        (void)app_node_comm_cancel_delivery(delivery_handle);
        return ret;
    }
    ret = gateway_begin_command_result_wait_for(
        &outbound->packet,
        command_id,
        control_timeout_ms);
    if (ret < 0) {
        enum node_transaction_action action;

        (void)app_node_comm_cancel_delivery(delivery_handle);
        (void)node_transaction_cancel(&gateway_survey_transaction.active,
                                      now_ms, &action);
        return ret;
    }
    ret = survey_gateway_auto_mark_waiting(&gateway_survey_auto);
    if (ret != PROTO_OK) {
        enum node_transaction_action action;

        (void)app_node_comm_cancel_delivery(delivery_handle);
        (void)node_transaction_cancel(&gateway_survey_transaction.active,
                                      now_ms, &action);
        gateway_clear_pending_command_result(&outbound->packet);
        return mesh_errno_from_proto(ret);
    }

    gateway_survey_pending_command = outbound->packet;
    gateway_survey_pending_command_valid = true;
    memset(&gateway_survey_result_preflight, 0,
           sizeof(gateway_survey_result_preflight));
    app_stack_workload_diag_gateway_control_admit(&outbound->packet, 1u, 1u);
    LOG_INF("gateway survey transaction submitted: cmd=0x%04x dst=0x%016llx seq=%u handle=%u deadline=%llu reason=%s",
            (unsigned int)command_id,
            (unsigned long long)outbound->packet.dst_id,
            outbound->packet.seq,
            delivery_handle,
            (unsigned long long)absolute_deadline_ms,
            reason == NULL ? "survey-control" : reason);
    (void)k_work_reschedule(&gateway_survey_work,
                            K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
    return 0;
#else
    ARG_UNUSED(outbound);
    ARG_UNUSED(command_id);
    ARG_UNUSED(reason);
    return -ENOTSUP;
#endif
}

static int gateway_survey_auto_send_prepare(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    ret = survey_init_pair_prepare_packet(&outbound.packet,
                                          &action->pair,
                                          DEVICE_ID,
                                          seq,
                                          (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = action->target_id;
    outbound.payload_len = (uint8_t)payload_len;

    LOG_INF("gateway survey auto prepare: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_PREPARE_PAIR,
                                             "survey-auto-prepare");
}

static int gateway_survey_auto_send_start(const struct survey_gateway_auto_action *action)
{
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint16_t seq;
    int ret;

    ret = mesh_append_command_id(outbound.payload,
                                 sizeof(outbound.payload),
                                 &payload_len,
                                 CMD_SURVEY_START_PAIR);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_append_pair_tlvs(outbound.payload,
                                  sizeof(outbound.payload),
                                  &payload_len,
                                  &action->pair);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    seq = gateway_next_command_seq();
    outbound.packet.msg_type = MSG_COMMAND;
    outbound.packet.src_id = DEVICE_ID;
    outbound.packet.dst_id = action->target_id;
    outbound.packet.session_id = action->pair.survey_id;
    outbound.packet.seq = seq;
    outbound.packet.ttl = MESH_DEFAULT_TTL;
    outbound.packet.payload_len = (uint8_t)payload_len;
    outbound.payload_len = (uint8_t)payload_len;

    LOG_INF("gateway survey auto start: survey=%u dst=0x%016llx seq=%u",
            action->pair.survey_id,
            (unsigned long long)action->target_id,
            seq);
    return gateway_survey_auto_send_outbound(&outbound,
                                             CMD_SURVEY_START_PAIR,
                                             "survey-auto-start");
}

static int gateway_survey_auto_send_action(const struct survey_gateway_auto_action *action)
{
    int ret;

    if (action == NULL) {
        return -EINVAL;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    ret = gateway_survey_transaction_load_action_pair(&action->pair);
    if (ret < 0) {
        return ret;
    }
#else
    ARG_UNUSED(ret);
#endif
    switch (action->command_id) {
    case CMD_SURVEY_PREPARE_PAIR:
        return gateway_survey_auto_send_prepare(action);
    case CMD_SURVEY_START_PAIR:
        return gateway_survey_auto_send_start(action);
    default:
        return -EINVAL;
    }
}

static void gateway_survey_auto_log_skipped_pair(const char *reason,
                                                 enum command_status status,
                                                 uint8_t detail,
                                                 enum gateway_command_event_reason event_reason)
{
    struct gateway_command_event event = gateway_observability_event(
        GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
        GATEWAY_COMMAND_EVENT_STAGE_PAIR_FAILURE,
        CMD_SURVEY_START_PAIR,
        &gateway_survey_host_command,
        gateway_survey_context.survey_id);

    gateway_survey_pair_failure_count++;
    event.status = status;
    event.reason = event_reason;
    event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
    event.pair_responder_id = gateway_survey_auto.pair.responder_id;
#if DEVICE_ROLE == ROLE_GATEWAY
    event.attempt = gateway_survey_transaction.active.request_attempts_started;
#endif
    event.success_count = gateway_survey_pair_success_count;
    event.failure_count = gateway_survey_pair_failure_count;
    event.duplicate_count = gateway_survey_duplicate_count;
    gateway_survey_terminal_failure_reason =
        gateway_command_survey_failure_reason_merge(
            gateway_survey_terminal_failure_reason,
            event.reason);
    (void)app_gateway_survey_observability_submit_boundary(
        &gateway_survey_observability, &gateway_survey_observability_ops,
        &event);
    LOG_WRN("gateway survey auto pair skipped: survey=%u initiator=0x%016llx responder=0x%016llx reason=%s status=%u detail=%u",
            gateway_survey_auto.pair.survey_id,
            (unsigned long long)gateway_survey_auto.pair.initiator_id,
            (unsigned long long)gateway_survey_auto.pair.responder_id,
            reason,
            status,
            detail);
    (void)k_work_reschedule(&gateway_survey_work, K_MSEC(GATEWAY_SURVEY_AUTO_RETRY_MS));
}

#if DEVICE_ROLE == ROLE_GATEWAY
static uint64_t gateway_survey_cleanup_target(uint8_t peer_mask)
{
    if (peer_mask == SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK) {
        return gateway_survey_transaction.pair.initiator_id;
    }
    if (peer_mask == SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK) {
        return gateway_survey_transaction.pair.responder_id;
    }
    return 0u;
}

static int gateway_survey_prepare_cleanup_delivery(
    struct gateway_survey_cleanup_delivery *cleanup,
    uint8_t peer_mask,
    uint64_t now_ms)
{
    uint64_t target_id = gateway_survey_cleanup_target(peer_mask);
    if (cleanup == NULL || target_id == 0u) {
        return -EINVAL;
    }
    memset(cleanup, 0, sizeof(*cleanup));
    cleanup->pair = gateway_survey_transaction.pair;
    cleanup->target_id = target_id;
    cleanup->sequence = gateway_next_command_seq();
    cleanup->absolute_deadline_ms =
        now_ms + SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
    cleanup->client_token = gateway_survey_next_transaction_token();
    cleanup->peer_mask = peer_mask;
    cleanup->prepared = true;
    return 0;
}

static int gateway_survey_build_cleanup_outbound(
    const struct gateway_survey_cleanup_delivery *cleanup,
    struct mesh_outbound *outbound)
{
    size_t payload_len = 0u;
    int ret;

    if (cleanup == NULL || outbound == NULL || !cleanup->prepared) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = mesh_append_command_id(outbound->payload,
                                 sizeof(outbound->payload),
                                 &payload_len,
                                 CMD_SURVEY_ABORT);
    if (ret == PROTO_OK) {
        ret = survey_append_pair_tlvs(outbound->payload,
                                      sizeof(outbound->payload),
                                      &payload_len,
                                      &cleanup->pair);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound->packet.msg_type = MSG_COMMAND;
    outbound->packet.src_id = DEVICE_ID;
    outbound->packet.dst_id = cleanup->target_id;
    outbound->packet.session_id = cleanup->pair.survey_id;
    outbound->packet.seq = cleanup->sequence;
    outbound->packet.ttl = MESH_DEFAULT_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint8_t)payload_len;
    return gateway_survey_prepare_pair_control(outbound);
}

static bool gateway_survey_cleanup_slots_active(void)
{
    return gateway_survey_cleanup.prepared;
}

static bool gateway_survey_cleanup_pending(void)
{
    return survey_gateway_transaction_cleanup_pending(
               &gateway_survey_transaction) ||
           gateway_survey_cleanup_slots_active();
}

static struct survey_gateway_drive_state gateway_survey_drive_state(void)
{
    return (struct survey_gateway_drive_state) {
        .survey_active = gateway_survey_active,
        .auto_running = gateway_survey_auto.running,
        .auto_waiting = gateway_survey_auto.waiting,
        .pair_observation_active =
            gateway_survey_pair_observation_active,
        .cleanup_pending = gateway_survey_cleanup_pending(),
        .boundary_pending = gateway_survey_observability.boundary_pending,
    };
}

static void gateway_survey_schedule_drive(void)
{
    struct survey_gateway_drive_state state = gateway_survey_drive_state();
    enum survey_gateway_drive_action action =
        survey_gateway_drive_action(&state);

    if (action == SURVEY_GATEWAY_DRIVE_POLL_CLEANUP) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
    } else if (action == SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_BLE_TX_RETRY_MS));
    } else if (action == SURVEY_GATEWAY_DRIVE_RUN_NOW) {
        status_debug_printf("DBG_SURVEY_DRIVE_RESUME survey=%u stage=%u\n",
                            gateway_survey_context.survey_id,
                            (unsigned int)gateway_survey_auto.stage);
        (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
    }
}

static void gateway_survey_finish_cleanup_if_complete(uint64_t now_ms)
{
    if (survey_gateway_transaction_cleanup_mask(
            &gateway_survey_transaction) != 0u ||
        gateway_survey_cleanup_slots_active()) {
        return;
    }
    if (gateway_survey_transaction.abandoning) {
        (void)survey_gateway_transaction_note_cleanup_complete(
            &gateway_survey_transaction, 0u, now_ms);
    }
    survey_gateway_transaction_pair_complete(&gateway_survey_transaction,
                                             true, now_ms);
}

static void gateway_survey_begin_cleanup(void)
{
    uint8_t cleanup_mask = survey_gateway_transaction_cleanup_mask(
        &gateway_survey_transaction);
    uint64_t now_ms = (uint64_t)k_uptime_get();
    uint8_t peer_mask;

    if (cleanup_mask == 0u) {
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (gateway_survey_cleanup.prepared) {
        return;
    }
    peer_mask = (cleanup_mask &
                 SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK) != 0u ?
                SURVEY_GATEWAY_TRANSACTION_INITIATOR_MASK :
                SURVEY_GATEWAY_TRANSACTION_RESPONDER_MASK;
    if (gateway_survey_prepare_cleanup_delivery(&gateway_survey_cleanup,
                                                peer_mask,
                                                now_ms) < 0) {
        LOG_ERR("gateway survey cleanup build failed: peer_mask=0x%02x",
                peer_mask);
    }
    (void)k_work_reschedule(&gateway_survey_work,
                            K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
}

static void gateway_survey_service_cleanup(void)
{
    struct gateway_survey_cleanup_delivery *cleanup =
        &gateway_survey_cleanup;
    struct mesh_outbound outbound;
    struct node_comm_terminal_event event;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int ret;

    if (!cleanup->prepared) {
        gateway_survey_begin_cleanup();
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (cleanup->submitted) {
        if (!app_node_comm_take_delivery_event_for(cleanup->handle, &event)) {
            return;
        }
        status_debug_printf("DBG_SURVEY_CLEANUP_TERMINAL dst=0x%016llx handle=%u reason=%u attempts=%u\n",
                            (unsigned long long)cleanup->target_id,
                            cleanup->handle,
                            (unsigned int)event.reason,
                            event.attempts_started);
        (void)survey_gateway_transaction_note_cleanup_complete(
            &gateway_survey_transaction, cleanup->peer_mask, now_ms);
        memset(cleanup, 0, sizeof(*cleanup));
        gateway_survey_begin_cleanup();
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    if (now_ms >= cleanup->absolute_deadline_ms) {
        LOG_ERR("gateway survey cleanup submission expired: dst=0x%016llx",
                (unsigned long long)cleanup->target_id);
        (void)survey_gateway_transaction_note_cleanup_complete(
            &gateway_survey_transaction,
            cleanup->peer_mask,
            now_ms);
        memset(cleanup, 0, sizeof(*cleanup));
        gateway_survey_begin_cleanup();
        gateway_survey_finish_cleanup_if_complete(now_ms);
        return;
    }
    ret = gateway_survey_build_cleanup_outbound(cleanup, &outbound);
    if (ret < 0) {
        return;
    }
    ret = app_node_comm_submit_delivery(
        &outbound,
        NODE_COMM_PROFILE_BOUNDED_CONTROL_FLOOD,
        cleanup->absolute_deadline_ms,
        cleanup->client_token,
        &cleanup->handle);
    if (ret < 0) {
        return;
    }
    cleanup->submitted = true;
    (void)survey_gateway_transaction_note_cleanup_started(
        &gateway_survey_transaction, cleanup->peer_mask);
    status_debug_printf("DBG_SURVEY_CLEANUP_SUBMIT dst=0x%016llx seq=%u handle=%u deadline=%llu\n",
                        (unsigned long long)cleanup->target_id,
                        cleanup->sequence,
                        cleanup->handle,
                        (unsigned long long)cleanup->absolute_deadline_ms);
}

static int gateway_survey_cancel_take_active_delivery(
    enum node_transaction_action *action)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    struct node_comm_terminal_event event;
    uint32_t handle;
    int ret;

    if (action == NULL || transaction->state == NODE_TRANSACTION_EMPTY) {
        return -EINVAL;
    }
    if (transaction->request_delivery_terminal) {
        *action = transaction->state == NODE_TRANSACTION_SUCCEEDED ?
                      NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS :
                  transaction->state == NODE_TRANSACTION_ABANDONING ?
                      NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED :
                  transaction->state == NODE_TRANSACTION_ABANDONED ?
                      NODE_TRANSACTION_ACTION_TERMINAL_ABANDON :
                      NODE_TRANSACTION_ACTION_WAIT_RESULT;
        return 0;
    }

    handle = transaction->request_delivery_handle;
    ret = app_node_comm_cancel_delivery(handle);
    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
    if (!app_node_comm_take_delivery_event_for(handle, &event)) {
        return -EAGAIN;
    }
    ret = survey_gateway_transaction_note_delivery_terminal(
        &gateway_survey_transaction, &event, (uint64_t)k_uptime_get(),
        action);
    if (ret < 0) {
        return ret;
    }
    status_debug_printf("DBG_SURVEY_CONTROL_CANCEL_TAKE handle=%u attempts=%u state=%u\n",
                        event.handle,
                        event.attempts_started,
                        (unsigned int)transaction->state);
    return 0;
}

static void gateway_survey_abandon_current(
    enum command_status status,
    uint8_t reason,
    enum gateway_command_event_reason event_reason,
    const char *log_reason)
{
    bool pair_launched = false;
    bool pair_skipped = false;
    enum command_id command_id =
        gateway_survey_transaction.active_command_id;
    enum node_transaction_action action;
    uint64_t now_ms = (uint64_t)k_uptime_get();
    int ret;

    if (gateway_survey_transaction.active.state != NODE_TRANSACTION_EMPTY &&
        !gateway_survey_transaction.active.request_delivery_terminal) {
        ret = gateway_survey_cancel_take_active_delivery(&action);
        if (ret < 0) {
            LOG_ERR("gateway survey active delivery cancel/take failed: ret=%d",
                    ret);
        }
    }
    survey_gateway_transaction_require_cleanup(&gateway_survey_transaction,
                                               false, now_ms);
    if (gateway_survey_pending_command_valid) {
        app_stack_workload_diag_gateway_control_release(
            &gateway_survey_pending_command, -ECANCELED, 0u, 0u);
        gateway_clear_pending_command_result(&gateway_survey_pending_command);
        gateway_survey_pending_command_valid = false;
    }
    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          gateway_survey_transaction.active_target_id,
                                          gateway_survey_transaction.pair.survey_id,
                                          status,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_OK && pair_skipped) {
        gateway_survey_auto_log_skipped_pair(log_reason,
                                             status,
                                             reason,
                                             event_reason);
    }
    gateway_survey_begin_cleanup();
}
#endif

bool gateway_survey_auto_preflight_result(const struct proto_packet *packet,
                                          const uint8_t *payload,
                                          size_t payload_len)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    struct node_transaction_key key;
    enum survey_gateway_transaction_result transaction_result;
    enum node_transaction_action action;
    enum command_id command_id = CMD_VENDOR_BASE;
    enum command_status status = COMMAND_INTERNAL_ERROR;
    uint32_t request_fingerprint = 0u;
    uint32_t result_fingerprint;
    uint8_t reason = 0u;
    int ret;

    if (!gateway_survey_active || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        gateway_command_extract_id(payload, payload_len, &command_id) !=
            PROTO_OK ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        return false;
    }
    key = (struct node_transaction_key) {
        .requester_id = packet->dst_id,
        .responder_id = packet->src_id,
        .session_id = packet->session_id,
        .transaction_id = packet->seq,
        .operation_id = (uint16_t)command_id,
    };
    if (!survey_gateway_transaction_request_fingerprint(
            &gateway_survey_transaction, &key, &request_fingerprint)) {
        return false;
    }
    result_fingerprint = node_transaction_fingerprint_bytes(0u,
                                                            payload,
                                                            payload_len);
    ret = app_mesh_gateway_command_flow_decode_result(command_id,
                                                      payload,
                                                      payload_len,
                                                      &status,
                                                      &reason);
    if (ret != PROTO_OK) {
        status = COMMAND_INTERNAL_ERROR;
        reason = (uint8_t)(-ret);
    }
    ret = survey_gateway_transaction_reconcile_result(
        &gateway_survey_transaction,
        &key,
        request_fingerprint,
        result_fingerprint,
        result_fingerprint,
        status,
        (uint64_t)k_uptime_get(),
        &transaction_result,
        &action);
    if (ret < 0) {
        return false;
    }
    gateway_survey_result_preflight =
        (struct gateway_survey_result_preflight) {
            .key = key,
            .result = transaction_result,
            .status = status,
            .reason = reason,
            .valid = true,
        };

    if (transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT) {
        if (gateway_survey_auto.waiting) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                reason,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "conflicting-command-result");
        } else {
            gateway_survey_auto.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
            gateway_survey_auto.waiting = false;
            gateway_survey_auto_log_skipped_pair(
                "conflicting-command-result",
                COMMAND_INTERNAL_ERROR,
                reason,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
            gateway_survey_begin_cleanup();
        }
    } else if (transaction_result ==
                   SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE) {
        gateway_survey_duplicate_count++;
    }
    return true;
#else
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return false;
#endif
}

bool gateway_survey_auto_owns_pending_command(
    const struct proto_packet *command,
    enum command_id command_id)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    if (command == NULL || !gateway_survey_active ||
        !gateway_survey_pending_command_valid ||
        !gateway_survey_auto.waiting ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        return false;
    }
    if (command->msg_type != gateway_survey_pending_command.msg_type ||
        command->src_id != gateway_survey_pending_command.src_id ||
        command->dst_id != gateway_survey_pending_command.dst_id ||
        command->session_id != gateway_survey_pending_command.session_id ||
        command->seq != gateway_survey_pending_command.seq) {
        return false;
    }
    return survey_gateway_auto_command_matches(&gateway_survey_auto,
                                                command_id,
                                                command->dst_id,
                                                command->session_id);
#else
    ARG_UNUSED(command);
    ARG_UNUSED(command_id);
    return false;
#endif
}

#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_survey_complete_accepted_delivery(void)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    enum node_transaction_action action;
    int ret;

    if (transaction->state != NODE_TRANSACTION_SUCCEEDED) {
        return -EINVAL;
    }
    if (transaction->request_delivery_terminal) {
        return 0;
    }

    ret = gateway_survey_cancel_take_active_delivery(&action);
    if (ret < 0) {
        return ret;
    }
    if (action != NODE_TRANSACTION_ACTION_TERMINAL_SUCCESS ||
        !transaction->request_delivery_terminal) {
        return -EPROTO;
    }
    return 0;
}
#endif

static void gateway_survey_auto_note_command_result(const struct proto_packet *command,
                                                    enum command_id command_id,
                                                    enum command_status status,
                                                    uint8_t reason)
{
    bool pair_launched = false;
    bool pair_skipped = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    enum survey_gateway_transaction_result transaction_result =
        SURVEY_GATEWAY_TRANSACTION_RESULT_STALE;
#endif
    int ret;

    if (!gateway_survey_auto_owns_pending_command(command, command_id)) {
        return;
    }

#if DEVICE_ROLE == ROLE_GATEWAY
    if (gateway_survey_result_preflight.valid &&
        gateway_survey_result_preflight.key.requester_id == command->src_id &&
        gateway_survey_result_preflight.key.responder_id == command->dst_id &&
        gateway_survey_result_preflight.key.session_id == command->session_id &&
        gateway_survey_result_preflight.key.transaction_id == command->seq &&
        gateway_survey_result_preflight.key.operation_id == (uint16_t)command_id) {
        transaction_result = gateway_survey_result_preflight.result;
        status = gateway_survey_result_preflight.status;
        reason = gateway_survey_result_preflight.reason;
        memset(&gateway_survey_result_preflight, 0,
               sizeof(gateway_survey_result_preflight));
    }
    if (transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_DUPLICATE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_LATE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_STALE ||
        transaction_result == SURVEY_GATEWAY_TRANSACTION_RESULT_CONFLICT) {
        return;
    }
    ret = gateway_survey_complete_accepted_delivery();
    if (ret < 0) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "accepted-delivery-close-failed");
        return;
    }
#endif

    app_stack_workload_diag_gateway_control_release(
        command, status == COMMAND_OK ? 0 : -EIO, 0u, 0u);
    gateway_survey_pending_command_valid = false;

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          status,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto result transition failed: cmd=0x%04x dst=0x%016llx status=%u ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                status,
                ret);
        gateway_survey_auto_finish();
        return;
    }

    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair(
            "command-result",
            status,
            reason,
            status == COMMAND_TIMEOUT ?
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                GATEWAY_COMMAND_EVENT_REASON_RADIO);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    } else if (pair_launched) {
#if DEVICE_ROLE == ROLE_GATEWAY
        if (survey_gateway_transaction_phase_complete(
                &gateway_survey_transaction) < 0) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                1u,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "transaction-retire-failed");
            return;
        }
#endif
        gateway_survey_pair_observation_active = true;
        gateway_survey_pair_result_mask = 0u;
        gateway_survey_pair_range_failure_count = 0u;
        {
            struct gateway_command_event event = gateway_observability_event(
                GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY,
                GATEWAY_COMMAND_EVENT_STAGE_PAIR_START,
                CMD_SURVEY_START_PAIR,
                &gateway_survey_host_command,
                gateway_survey_context.survey_id);

            event.pair_initiator_id = gateway_survey_auto.pair.initiator_id;
            event.pair_responder_id = gateway_survey_auto.pair.responder_id;
            event.progress_count =
                (uint16_t)gateway_survey_context.next_pair_index;
            event.total_count = (uint16_t)gateway_survey_context.pair_count;
            (void)app_gateway_survey_observability_submit_boundary(
                &gateway_survey_observability,
                &gateway_survey_observability_ops, &event);
        }
        LOG_INF("gateway survey pair launched: survey=%u initiator=0x%016llx responder=0x%016llx samples=%u",
                gateway_survey_auto.pair.survey_id,
                (unsigned long long)gateway_survey_auto.pair.initiator_id,
                (unsigned long long)gateway_survey_auto.pair.responder_id,
                gateway_survey_auto.pair.sample_count);
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(gateway_survey_pair_run_delay_ms(&gateway_survey_auto.pair)));
    } else {
#if DEVICE_ROLE == ROLE_GATEWAY
        if (survey_gateway_transaction_phase_complete(
                &gateway_survey_transaction) < 0) {
            gateway_survey_abandon_current(
                COMMAND_INTERNAL_ERROR,
                1u,
                GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
                "transaction-retire-failed");
            return;
        }
#endif
        (void)k_work_reschedule(&gateway_survey_work, K_NO_WAIT);
    }
}

static void gateway_survey_auto_note_command_timeout(const struct proto_packet *command,
                                                     enum command_id command_id)
{
    bool pair_launched = false;
    bool pair_skipped = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    enum node_transaction_action action;
#endif
    int ret;

    if (!gateway_survey_auto_owns_pending_command(command, command_id)) {
        return;
    }

    app_stack_workload_diag_gateway_control_release(command, -ETIMEDOUT,
                                                    0u, 0u);
    gateway_survey_pending_command_valid = false;
#if DEVICE_ROLE == ROLE_GATEWAY
    (void)survey_gateway_transaction_service(&gateway_survey_transaction,
                                             (uint64_t)k_uptime_get(),
                                             &action);
    survey_gateway_transaction_require_cleanup(&gateway_survey_transaction,
                                               false,
                                               (uint64_t)k_uptime_get());
#endif

    ret = survey_gateway_auto_note_result(&gateway_survey_auto,
                                          command_id,
                                          command->dst_id,
                                          command->session_id,
                                          COMMAND_TIMEOUT,
                                          &pair_launched,
                                          &pair_skipped);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey auto timeout transition failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)command->dst_id,
                ret);
        gateway_survey_auto_finish();
        return;
    }
    if (pair_skipped) {
        gateway_survey_auto_log_skipped_pair(
            "command-timeout",
            COMMAND_TIMEOUT,
            0u,
            GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    }
}

#if DEVICE_ROLE == ROLE_GATEWAY
static uint32_t gateway_discovery_assignment_new_epoch(void)
{
    uint32_t epoch = sys_rand32_get() ^ k_uptime_get_32();

    return epoch == 0u ? 1u : epoch;
}

static uint32_t gateway_discovery_assignment_next_generation(void)
{
    gateway_discovery_assignment_generation++;
    if (gateway_discovery_assignment_generation == 0u) {
        gateway_discovery_assignment_generation = 1u;
    }
    return gateway_discovery_assignment_generation;
}

static int gateway_build_discovery_assignment_command(
    struct mesh_outbound *outbound,
    enum discovery_assignment_phase phase,
    uint32_t epoch,
    uint32_t command_seq)
{
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || epoch == 0u || command_seq == 0u) {
        return -EINVAL;
    }
    memset(outbound, 0, sizeof(*outbound));
    ret = tlv_append_u16(outbound->payload,
                         sizeof(outbound->payload),
                         &payload_len,
                         TLV_COMMAND_ID,
                         CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_SEQ,
                             command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_EXPIRY_S,
                             DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(outbound->payload,
                                                       sizeof(outbound->payload),
                                                       &payload_len,
                                                       phase,
                                                       epoch);
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    outbound->packet.msg_type = MSG_COMMAND;
    outbound->packet.flags = FLAG_DIAGNOSTIC;
    outbound->packet.src_id = DEVICE_ID;
    outbound->packet.dst_id = MESH_BROADCAST_ID;
    outbound->packet.session_id = command_seq;
    outbound->packet.seq = gateway_next_command_seq();
    outbound->packet.ttl = FLOOD_EPOCH_GLOBAL_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = gateway_command_append_default_flood_controls(outbound);
    return ret == PROTO_OK ? 0 : mesh_errno_from_proto(ret);
}

static int gateway_send_discovery_assignment_claim_request(void)
{
    struct mesh_outbound outbound;
    bool sent_now = false;
    int ret;

    ret = gateway_build_discovery_assignment_command(
        &outbound,
        DISCOVERY_ASSIGNMENT_PHASE_CLAIM,
        gateway_discovery_assignment_state.epoch,
        gateway_discovery_assignment_state.claim_command_seq);
    if (ret < 0) {
        return ret;
    }
    ret = app_node_comm_send_control_flood(
        &outbound,
        C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
        "discovery-slot-claim-request",
        &sent_now);
    if (ret == 0 && sent_now) {
        mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REQUEST epoch=%u round=%u command_seq=%u sent_now=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.claim_round,
                        gateway_discovery_assignment_state.claim_command_seq,
                        sent_now ? 1u : 0u,
                        ret);
    return ret;
}

static uint8_t gateway_discovery_assignment_table_round_limit(void);

static uint32_t gateway_discovery_assignment_window_ms(void)
{
    uint32_t natural_window_ms = discovery_assignment_collection_window_ms(
        UWB_DISCOVERY_SLOT_COUNT,
        gateway_discovery_assignment_state.max_hop_count);
    uint32_t remaining_ms = uptime_ms_until_deadline(
        k_uptime_get_32(),
        gateway_discovery_assignment_state.operation_deadline_ms);
    bool collecting_claims =
        gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;

    if (collecting_claims) {
        return gateway_command_budget_weighted_window_ms(
            gateway_discovery_assignment_state.budget_explicit,
            remaining_ms,
            3u,
            5u,
            natural_window_ms);
    }
    uint8_t windows_remaining =
        app_discovery_assignment_table_windows_remaining(
            gateway_discovery_assignment_state.table_round,
            gateway_discovery_assignment_table_round_limit());

    return gateway_command_budget_window_ms(
        gateway_discovery_assignment_state.budget_explicit,
        remaining_ms,
        windows_remaining,
        natural_window_ms);
}

static uint8_t gateway_discovery_assignment_claim_round_limit(void)
{
    uint8_t budget_round_limit = gateway_command_budget_retry_limit(
        gateway_discovery_assignment_state.budget_explicit,
        gateway_discovery_assignment_state.command_budget_ms,
        DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);

    return app_discovery_assignment_claim_round_limit(
        gateway_discovery_assignment_state.budget_explicit,
        budget_round_limit,
        DISCOVERY_ASSIGNMENT_CLAIM_MAX_ROUNDS);
}

static uint8_t gateway_discovery_assignment_table_round_limit(void)
{
    return gateway_command_budget_retry_limit(
        gateway_discovery_assignment_state.budget_explicit,
        gateway_discovery_assignment_state.command_budget_ms,
        DISCOVERY_ASSIGNMENT_TABLE_MAX_ROUNDS);
}

static uint32_t gateway_discovery_assignment_next_command_seq(uint32_t current)
{
    current++;
    return current == 0u ? 1u : current;
}

static uint64_t gateway_discovery_assignment_expected_ack_mask(void)
{
    size_t count = gateway_discovery_assignment_state.claim_count;

    return count >= 64u ? UINT64_MAX : (UINT64_C(1) << count) - 1u;
}

static uint8_t gateway_discovery_assignment_missing_ack_count(void)
{
    uint64_t missing = gateway_discovery_assignment_expected_ack_mask() &
                       ~gateway_discovery_assignment_state.ack_mask;
    uint8_t count = 0u;

    while (missing != 0u) {
        count += (uint8_t)(missing & 1u);
        missing >>= 1;
    }
    return count;
}

static int gateway_discovery_assignment_open_claim_round(void)
{
    uint32_t wait_ms;
    int ret;

    if (gateway_discovery_assignment_state.claim_round >=
        gateway_discovery_assignment_claim_round_limit()) {
        return -ETIMEDOUT;
    }
    gateway_discovery_assignment_state.claim_round++;
    gateway_discovery_assignment_state.claim_count_at_round_start =
        gateway_discovery_assignment_state.claim_count;
    gateway_discovery_assignment_state.claim_command_seq =
        gateway_discovery_assignment_next_command_seq(
            gateway_discovery_assignment_state.claim_command_seq);
    ret = gateway_send_discovery_assignment_claim_request();
    gateway_discovery_assignment_state.round_open = ret == 0;
    wait_ms = ret == 0 ? gateway_discovery_assignment_window_ms() :
              discovery_assignment_retry_backoff_ms(
                  gateway_discovery_assignment_state.claim_round - 1u,
                  sys_rand32_get());
    (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                            K_MSEC(wait_ms));
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            ret == 0 ? GATEWAY_COMMAND_EVENT_STAGE_FLOOD_ATTEMPT :
                       GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.attempt = gateway_discovery_assignment_state.claim_round;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        event.status = ret == 0 ? COMMAND_OK : COMMAND_RADIO_ERROR;
        event.reason = ret == 0 ? GATEWAY_COMMAND_EVENT_REASON_NONE :
                                 GATEWAY_COMMAND_EVENT_REASON_RADIO;
        (void)gateway_observe_command_event(&event, false);
    }
    return ret;
}

static void gateway_discovery_assignment_fail(enum command_status status,
                                              uint8_t reason)
{
    if (!gateway_discovery_assignment_state.active) {
        return;
    }
    gateway_emit_host_command_result(&gateway_discovery_assignment_state.host_command,
                                     CMD_ASSIGN_DISCOVERY_SLOTS,
                                     status,
                                     reason);
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.status = status;
        event.reason = gateway_discovery_assignment_state.claim_count == 0u ?
                       GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS :
                       status == COMMAND_TIMEOUT ?
                       GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                       GATEWAY_COMMAND_EVENT_REASON_RADIO;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.total_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.failure_count = reason;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, true);
    }
    gateway_discovery_assignment_state.active = false;
    gateway_discovery_assignment_state.round_open = false;
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_finalize_work);
    (void)k_work_cancel_delayable(&gateway_discovery_assignment_publish_work);
}

static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    uint32_t budget_ms = 90000u;
    bool budget_explicit = false;
    int ret;

    if (host_command == NULL || payload == NULL || DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }
    if (gateway_discovery_assignment_state.active) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_BUSY,
                                         1u);
        return -EBUSY;
    }
    ret = gateway_command_extract_budget_ms(
        payload, payload_len, 90000u, &budget_ms, &budget_explicit);
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_command,
                                         CMD_ASSIGN_DISCOVERY_SLOTS,
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    memset(&gateway_discovery_assignment_state,
           0,
           sizeof(gateway_discovery_assignment_state));
    gateway_discovery_assignment_state.host_command = *host_command;
    gateway_discovery_assignment_state.epoch =
        gateway_discovery_assignment_new_epoch();
    gateway_discovery_assignment_state.operation_deadline_ms =
        k_uptime_get_32() + budget_ms;
    gateway_discovery_assignment_state.command_budget_ms = budget_ms;
    gateway_discovery_assignment_state.budget_explicit = budget_explicit;
    gateway_discovery_assignment_state.generation =
        gateway_discovery_assignment_next_generation();
    gateway_discovery_assignment_state.claim_command_seq =
        gateway_discovery_assignment_state.epoch - 1u;
    gateway_discovery_assignment_state.stage =
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;
    gateway_discovery_assignment_state.active = true;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            host_command,
            gateway_discovery_assignment_state.epoch);

        (void)gateway_observe_command_event(&event, false);
    }

    ret = gateway_discovery_assignment_open_claim_round();
    LOG_INF("gateway discovery-slot collection started: epoch=%u window_ms=%u ret=%d",
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_window_ms(),
            ret);
    return 0;
}

bool gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id)
{
    enum discovery_assignment_phase phase = 0;
    enum command_id command_id = CMD_VENDOR_BASE;
    const uint8_t *hop_raw = NULL;
    const uint8_t *status_raw = NULL;
    uint8_t hop_len = 0u;
    uint8_t status_len = 0u;
    uint8_t hop_count = 0u;
    uint64_t hash = 0u;
    uint32_t epoch = 0u;
    size_t anchor_index = SIZE_MAX;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL || payload == NULL ||
        packet->msg_type != MSG_COMMAND_RESULT ||
        !gateway_discovery_assignment_state.active) {
        return false;
    }
    ret = gateway_command_extract_id(payload, payload_len, &command_id);
    if (ret != PROTO_OK || command_id != CMD_ASSIGN_DISCOVERY_SLOTS) {
        return false;
    }
    ret = discovery_assignment_extract_control_tlvs(payload,
                                                    payload_len,
                                                    &phase,
                                                    &epoch);
    if (ret != PROTO_OK ||
        (phase != DISCOVERY_ASSIGNMENT_PHASE_CLAIM &&
         phase != DISCOVERY_ASSIGNMENT_PHASE_ACK) ||
        epoch != gateway_discovery_assignment_state.epoch) {
        return false;
    }
    ret = tlv_find(payload,
                   payload_len,
                   TLV_COMMAND_STATUS,
                   &status_raw,
                   &status_len);
    if (ret != PROTO_OK || status_len != sizeof(uint16_t) ||
        proto_get_u16_le(status_raw) != COMMAND_OK ||
        discovery_assignment_extract_claim_hash(payload,
                                                payload_len,
                                                &hash) != PROTO_OK ||
        packet->src_id == 0u ||
        hash != discovery_assignment_hash(packet->src_id)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u ret=%d\n",
                            (unsigned long long)packet->src_id,
                            epoch,
                            ret);
        return true;
    }
    if (tlv_find(payload, payload_len, TLV_HOP_COUNT, &hop_raw, &hop_len) ==
        PROTO_OK && hop_len == sizeof(uint8_t)) {
        hop_count = hop_raw[0];
        if (hop_count > gateway_discovery_assignment_state.max_hop_count) {
            gateway_discovery_assignment_state.max_hop_count = hop_count;
        }
    }
    for (size_t i = 0u;
         i < gateway_discovery_assignment_state.claim_count;
         i++) {
        if (gateway_discovery_assignment_state.anchor_ids[i] == packet->src_id) {
            anchor_index = i;
            break;
        }
    }

    if (phase == DISCOVERY_ASSIGNMENT_PHASE_ACK) {
        if (gateway_discovery_assignment_state.stage !=
                GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS ||
            packet->session_id !=
                gateway_discovery_assignment_state.table_command_seq ||
            anchor_index == SIZE_MAX) {
            status_debug_printf("DBG_DISCOVERY_SLOT_ACK_REJECT src=0x%016llx epoch=%u session=%u expected=%u index=%d\n",
                                (unsigned long long)packet->src_id,
                                epoch,
                                packet->session_id,
                                gateway_discovery_assignment_state.table_command_seq,
                                anchor_index == SIZE_MAX ? -1 : (int)anchor_index);
            return true;
        }
        gateway_discovery_assignment_state.ack_mask |=
            UINT64_C(1) << anchor_index;
        status_debug_printf("DBG_DISCOVERY_SLOT_ACK_RX epoch=%u anchor=0x%016llx hop=%u acked=%u missing=%u\n",
                            epoch,
                            (unsigned long long)packet->src_id,
                            hop_count,
                            (unsigned int)__builtin_popcountll(
                                gateway_discovery_assignment_state.ack_mask),
                            gateway_discovery_assignment_missing_ack_count());
        if (gateway_discovery_assignment_missing_ack_count() == 0u) {
            gateway_emit_host_command_result(
                &gateway_discovery_assignment_state.host_command,
                CMD_ASSIGN_DISCOVERY_SLOTS,
                COMMAND_OK,
                (uint8_t)gateway_discovery_assignment_state.claim_count);
            {
                struct gateway_command_event event = gateway_observability_event(
                    GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
                    GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
                    CMD_ASSIGN_DISCOVERY_SLOTS,
                    &gateway_discovery_assignment_state.host_command,
                    gateway_discovery_assignment_state.epoch);

                event.progress_count =
                    (uint16_t)gateway_discovery_assignment_state.claim_count;
                event.total_count = event.progress_count;
                event.success_count = event.progress_count;
                event.duplicate_count =
                    gateway_discovery_assignment_state.duplicate_count;
                (void)gateway_observe_command_event(&event, true);
            }
            gateway_discovery_assignment_state.active = false;
            gateway_discovery_assignment_state.round_open = false;
            (void)k_work_cancel_delayable(
                &gateway_discovery_assignment_finalize_work);
        }
        return true;
    }

    if (anchor_index != SIZE_MAX) {
        if (gateway_discovery_assignment_state.duplicate_count < UINT16_MAX) {
            gateway_discovery_assignment_state.duplicate_count++;
        }
        return true;
    }
    if (gateway_discovery_assignment_state.claim_count >=
        ARRAY_SIZE(gateway_discovery_assignment_state.anchor_ids)) {
        status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_REJECT src=0x%016llx epoch=%u reason=capacity\n",
                            (unsigned long long)packet->src_id,
                            epoch);
        return true;
    }
    gateway_discovery_assignment_state.anchor_ids[
        gateway_discovery_assignment_state.claim_count] = packet->src_id;
    gateway_discovery_assignment_state.claim_count++;
    {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.anchor_id = packet->src_id;
        event.previous_hop_id = previous_hop_id;
        event.hop_count = hop_count;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        (void)gateway_observe_command_event(&event, false);
    }
    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS) {
        gateway_discovery_assignment_state.stage =
            GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS;
        gateway_discovery_assignment_state.ack_mask = 0u;
        gateway_discovery_assignment_state.table_round = 0u;
        gateway_discovery_assignment_state.claim_round = 0u;
        gateway_discovery_assignment_state.round_open = false;
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_NO_WAIT);
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_RX epoch=%u anchor=0x%016llx hash=0x%016llx hop=%u count=%u\n",
                        epoch,
                        (unsigned long long)packet->src_id,
                        (unsigned long long)hash,
                        hop_count,
                        (unsigned int)gateway_discovery_assignment_state.claim_count);
    return true;
}

static int gateway_discovery_assignment_publish_table(void)
{
    struct mesh_outbound outbound;
    size_t payload_len;
    bool sent_now = false;
    int ret;

    if (gateway_discovery_assignment_state.claim_count == 0u) {
        return -ENOENT;
    }

    ret = discovery_assignment_sort_anchor_ids(
        gateway_discovery_assignment_state.anchor_ids,
        gateway_discovery_assignment_state.claim_count);
    if (ret == PROTO_OK) {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_ANCHOR_ENUMERATED,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);
        int publish_ret = app_gateway_assignment_publisher_stage_sorted_ids(
            &event,
            gateway_discovery_assignment_state.anchor_ids,
            gateway_discovery_assignment_state.claim_count,
            gateway_discovery_assignment_state.duplicate_count);

        if (publish_ret < 0) {
            LOG_WRN("discovery assignment telemetry batch unavailable: epoch=%u ret=%d",
                    gateway_discovery_assignment_state.epoch,
                    publish_ret);
        }
    }
    if (gateway_discovery_assignment_state.table_command_seq == 0u) {
        gateway_discovery_assignment_state.table_command_seq =
            gateway_discovery_assignment_state.epoch ^ UINT32_C(0x80000000);
        if (gateway_discovery_assignment_state.table_command_seq == 0u) {
            gateway_discovery_assignment_state.table_command_seq = 1u;
        }
    }
    if (ret == PROTO_OK) {
        ret = gateway_build_discovery_assignment_command(
            &outbound,
            DISCOVERY_ASSIGNMENT_PHASE_TABLE,
            gateway_discovery_assignment_state.epoch,
            gateway_discovery_assignment_state.table_command_seq);
    }
    payload_len = ret == 0 ? outbound.payload_len : 0u;
    if (ret == 0) {
        ret = discovery_assignment_append_table_from_anchor_ids(
            outbound.payload,
            sizeof(outbound.payload),
            &payload_len,
            gateway_discovery_assignment_state.anchor_ids,
            gateway_discovery_assignment_state.claim_count);
    }
    if (ret == PROTO_OK || ret == 0) {
        outbound.payload_len = (uint16_t)payload_len;
        outbound.packet.payload_len = (uint16_t)payload_len;
        ret = app_node_comm_send_control_flood(
            &outbound,
            C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
            "discovery-slot-assignment-table",
            &sent_now);
        if (ret == 0 && sent_now) {
            mesh_relay_note_tx_sent(&mesh_runtime,
                                    &outbound,
                                    k_uptime_get_32());
        }
    } else {
        ret = mesh_errno_from_proto(ret);
    }
    gateway_discovery_assignment_state.table_round++;
    if (ret == 0) {
        struct gateway_command_event event = gateway_observability_event(
            GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
            GATEWAY_COMMAND_EVENT_STAGE_SCHEDULE_READY,
            CMD_ASSIGN_DISCOVERY_SLOTS,
            &gateway_discovery_assignment_state.host_command,
            gateway_discovery_assignment_state.epoch);

        event.attempt = gateway_discovery_assignment_state.table_round;
        event.progress_count =
            (uint16_t)gateway_discovery_assignment_state.claim_count;
        event.total_count = event.progress_count;
        event.status = COMMAND_OK;
        event.reason = GATEWAY_COMMAND_EVENT_REASON_NONE;
        event.duplicate_count = gateway_discovery_assignment_state.duplicate_count;
        app_gateway_assignment_publisher_stage_table_ready(&event);
    }
    status_debug_printf("DBG_DISCOVERY_SLOT_TABLE_TX epoch=%u generation=%u count=%u bytes=%u round=%u sent_now=%u ret=%d\n",
                        gateway_discovery_assignment_state.epoch,
                        gateway_discovery_assignment_state.table_command_seq,
                        (unsigned int)gateway_discovery_assignment_state.claim_count,
                        (unsigned int)payload_len,
                        gateway_discovery_assignment_state.table_round,
                        sent_now ? 1u : 0u,
                        ret);
    gateway_discovery_assignment_state.stage =
        GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS;
    gateway_discovery_assignment_state.round_open = ret == 0;
    if (ret == 0) {
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(gateway_discovery_assignment_window_ms()));
    } else if (gateway_discovery_assignment_state.table_round <
               gateway_discovery_assignment_table_round_limit()) {
        (void)k_work_reschedule(
            &gateway_discovery_assignment_finalize_work,
            K_MSEC(discovery_assignment_retry_backoff_ms(
                gateway_discovery_assignment_state.table_round - 1u,
                sys_rand32_get())));
    }
    return ret;
}

static void gateway_discovery_assignment_publish_work_handler(struct k_work *work)
{
    bool current_generation;
    int ret;

    ARG_UNUSED(work);

    current_generation = app_discovery_assignment_work_guard_begin(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation);
    if (DEVICE_ROLE != ROLE_GATEWAY ||
        !gateway_discovery_assignment_state.active ||
        !current_generation) {
        return;
    }

    if (gateway_discovery_assignment_state.stage ==
        GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS) {
        if (!gateway_discovery_assignment_state.round_open) {
            if (gateway_discovery_assignment_state.claim_round <
                gateway_discovery_assignment_claim_round_limit()) {
                (void)gateway_discovery_assignment_open_claim_round();
                return;
            }
            if (gateway_discovery_assignment_state.claim_count == 0u) {
                gateway_discovery_assignment_fail(COMMAND_TIMEOUT, 0u);
                return;
            }
        } else {
            gateway_discovery_assignment_state.round_open = false;
        }

        if (gateway_discovery_assignment_state.claim_round <
            gateway_discovery_assignment_claim_round_limit()) {
            uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
                gateway_discovery_assignment_state.claim_round - 1u,
                sys_rand32_get());

            status_debug_printf("DBG_DISCOVERY_SLOT_CLAIM_BACKOFF epoch=%u round=%u count=%u delay=%u\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.claim_round,
                                (unsigned int)gateway_discovery_assignment_state.claim_count,
                                retry_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(retry_ms));
            return;
        }
        if (gateway_discovery_assignment_state.claim_count == 0u) {
            gateway_discovery_assignment_fail(COMMAND_TIMEOUT, 0u);
            return;
        }

        gateway_discovery_assignment_state.table_round = 0u;
        gateway_discovery_assignment_state.table_command_seq = 0u;
        ret = gateway_discovery_assignment_publish_table();
        if (ret < 0 && gateway_discovery_assignment_state.table_round >=
            gateway_discovery_assignment_table_round_limit()) {
            gateway_discovery_assignment_fail(COMMAND_RADIO_ERROR,
                                              (uint8_t)(-ret));
        }
        return;
    }

    if (gateway_discovery_assignment_missing_ack_count() == 0u) {
        return;
    }
    if (gateway_discovery_assignment_state.table_round >=
        gateway_discovery_assignment_table_round_limit()) {
        gateway_discovery_assignment_fail(
            COMMAND_TIMEOUT,
            gateway_discovery_assignment_missing_ack_count());
        return;
    }
    ret = gateway_discovery_assignment_publish_table();
    if (ret < 0 && gateway_discovery_assignment_state.table_round >=
        gateway_discovery_assignment_table_round_limit()) {
        gateway_discovery_assignment_fail(COMMAND_RADIO_ERROR,
                                          (uint8_t)(-ret));
    }
}

static void gateway_discovery_assignment_finalize_work_handler(struct k_work *work)
{
    enum app_discovery_assignment_work_request request;
    uint8_t missing_ack_count;
    uint8_t table_round_limit;
    int ret;

    ARG_UNUSED(work);

    if (!gateway_discovery_assignment_state.active) {
        return;
    }
    if (app_discovery_assignment_operation_expired(
            k_uptime_get_32(),
            gateway_discovery_assignment_state.operation_deadline_ms)) {
        gateway_discovery_assignment_fail(COMMAND_TIMEOUT, 0u);
        return;
    }
    missing_ack_count = gateway_discovery_assignment_missing_ack_count();
    table_round_limit = gateway_discovery_assignment_table_round_limit();
    if (gateway_discovery_assignment_state.stage ==
            GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS &&
        gateway_discovery_assignment_state.round_open) {
        gateway_discovery_assignment_state.round_open = false;
        if (app_discovery_assignment_table_retry_backoff_required(
                true,
                missing_ack_count,
                gateway_discovery_assignment_state.table_round,
                table_round_limit)) {
            uint32_t retry_ms = discovery_assignment_retry_backoff_ms(
                gateway_discovery_assignment_state.table_round - 1u,
                sys_rand32_get());

            status_debug_printf("DBG_DISCOVERY_SLOT_TABLE_BACKOFF epoch=%u round=%u missing=%u delay=%u\n",
                                gateway_discovery_assignment_state.epoch,
                                gateway_discovery_assignment_state.table_round,
                                missing_ack_count,
                                retry_ms);
            (void)k_work_reschedule(
                &gateway_discovery_assignment_finalize_work,
                K_MSEC(retry_ms));
            return;
        }
    }
    request = app_discovery_assignment_work_guard_request(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation);
    if (request == APP_DISCOVERY_ASSIGNMENT_WORK_ALREADY_PENDING) {
        return;
    }
    if (request != APP_DISCOVERY_ASSIGNMENT_WORK_SUBMIT) {
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_MSEC(100u));
        return;
    }
    ret = mesh_gateway_command_priority_submit(
        &gateway_discovery_assignment_publish_work);
    app_discovery_assignment_work_guard_note_submit_result(
        &gateway_discovery_assignment_publish_guard,
        gateway_discovery_assignment_state.generation,
        ret);
    if (ret < 0) {
        (void)k_work_reschedule(&gateway_discovery_assignment_finalize_work,
                                K_MSEC(100u));
    }
}
#else
static int gateway_start_discovery_assignment(
    const struct proto_packet *host_command,
    const uint8_t *payload,
    size_t payload_len)
{
    ARG_UNUSED(host_command);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    return -ENOTSUP;
}

bool gateway_discovery_assignment_note_claim(const struct proto_packet *packet,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             uint64_t previous_hop_id)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(previous_hop_id);
    return false;
}
#endif

void gateway_command_timeout_side_effects(const struct proto_packet *command,
                                          enum command_id command_id)
{
    if (command == NULL) {
        return;
    }

    mesh_clear_route_waiting_tx(command);
    gateway_survey_auto_note_command_timeout(command, command_id);
    if (command_id == CMD_FORCE_REDISCOVERY) {
        (void)app_node_comm_request_route_refresh(
            0u, "force-rediscovery-timeout", false);
    }
}

void gateway_command_result_side_effects(const struct proto_packet *command,
                                         enum command_id command_id,
                                         enum command_status status,
                                         uint8_t reason)
{
    gateway_survey_auto_note_command_result(command, command_id, status, reason);
    if (command_id == CMD_FORCE_REDISCOVERY && status == COMMAND_OK) {
        (void)app_node_comm_request_route_refresh(
            0u, "force-rediscovery-result", false);
    }
}

#if DEVICE_ROLE == ROLE_GATEWAY
static void gateway_survey_service_active_delivery(void)
{
    struct node_transaction *transaction =
        &gateway_survey_transaction.active;
    struct node_comm_terminal_event event;
    enum node_transaction_action action;
    int ret;

    if (transaction->state != NODE_TRANSACTION_ACTIVE ||
        transaction->request_delivery_terminal) {
        return;
    }
    if (!app_node_comm_take_delivery_event_for(
            transaction->request_delivery_handle, &event)) {
        (void)k_work_reschedule(&gateway_survey_work,
                                K_MSEC(GATEWAY_SURVEY_TRANSACTION_POLL_MS));
        return;
    }
    ret = survey_gateway_transaction_note_delivery_terminal(
        &gateway_survey_transaction, &event, (uint64_t)k_uptime_get(),
        &action);
    if (ret < 0) {
        gateway_survey_abandon_current(
            COMMAND_INTERNAL_ERROR,
            (uint8_t)(-ret),
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL,
            "delivery-terminal-mismatch");
        return;
    }
    app_stack_workload_diag_gateway_control_sample(
        &gateway_survey_pending_command,
        event.attempts_started,
        event.reason == NODE_COMM_TERMINAL_DELIVERED ? 1u : 0u);
    if (action == NODE_TRANSACTION_ACTION_CLEANUP_REQUIRED ||
        action == NODE_TRANSACTION_ACTION_TERMINAL_ABANDON) {
        gateway_survey_abandon_current(
            event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
            (uint8_t)event.reason,
            event.reason == NODE_COMM_TERMINAL_DEADLINE_EXPIRED ?
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                GATEWAY_COMMAND_EVENT_REASON_RADIO,
            "control-delivery-failed");
    }
}
#endif

static void gateway_survey_work_handler(struct k_work *work)
{
    struct survey_gateway_auto_action action = {0};
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return;
    }
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_service_cleanup();
    if (!gateway_survey_active) {
        goto out;
    }
    if (gateway_survey_budget_explicit &&
        uptime_deadline_reached(k_uptime_get_32(),
                                gateway_survey_operation_deadline_ms)) {
        gateway_survey_auto_finish_status(
            COMMAND_TIMEOUT, GATEWAY_COMMAND_EVENT_REASON_TIMEOUT);
        goto out;
    }
    gateway_survey_service_active_delivery();
    if (gateway_survey_cleanup_pending() &&
        (gateway_survey_pair_observation_active ||
         !survey_gateway_auto_no_unstarted_pairs(
             &gateway_survey_auto, &gateway_survey_context))) {
        goto out;
    }
#endif
    if (gateway_survey_auto.waiting) {
        goto out;
    }
    if (!gateway_survey_flush_boundary_event() ||
        !gateway_survey_finalize_pair_observation()) {
        goto out;
    }
    if (!gateway_survey_auto.running) {
        if (!gateway_survey_context.pairs_planned) {
            ret = survey_gateway_plan_pairs(&gateway_survey_context);
            if (ret != PROTO_OK) {
                LOG_WRN("gateway survey final pair planning failed: survey=%u ret=%d",
                        gateway_survey_context.survey_id,
                        ret);
                gateway_survey_auto_finish();
                goto out;
            }
        }
        if (!gateway_survey_emit_collection_telemetry()) {
            goto out;
        }
        LOG_INF("gateway survey orchestration starting: survey=%u reports=%u pairs=%u",
                gateway_survey_context.survey_id,
                (unsigned int)gateway_survey_context.report_count,
                (unsigned int)gateway_survey_context.pair_count);
    }

    ret = survey_gateway_auto_next_action(&gateway_survey_auto,
                                          &gateway_survey_context,
                                          &action);
    if (ret == PROTO_OK && action.complete) {
        gateway_survey_auto_finish();
        goto out;
    }
    if (ret == PROTO_ERR_BUSY) {
        goto out;
    }
    if (ret != PROTO_OK) {
        LOG_WRN("gateway survey next action failed: survey=%u ret=%d",
                gateway_survey_context.survey_id,
                ret);
        gateway_survey_auto_finish();
        goto out;
    }

    ret = gateway_survey_auto_send_action(&action);
    if (ret < 0) {
        LOG_WRN("gateway survey auto send failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)action.command_id,
                (unsigned long long)action.target_id,
                ret);
#if DEVICE_ROLE == ROLE_GATEWAY
        if (gateway_survey_transaction.active.state == NODE_TRANSACTION_ACTIVE) {
            (void)app_node_comm_cancel_delivery(
                gateway_survey_transaction.active.request_delivery_handle);
        }
        survey_gateway_transaction_require_cleanup(
            &gateway_survey_transaction, false, (uint64_t)k_uptime_get());
#endif
        gateway_survey_auto.waiting = false;
        gateway_survey_auto.stage = SURVEY_GATEWAY_AUTO_LOAD_PAIR;
        gateway_survey_auto_log_skipped_pair("send-failed",
            ret == -EHOSTUNREACH ? COMMAND_TIMEOUT : COMMAND_RADIO_ERROR,
            (uint8_t)(-ret),
            ret == -EHOSTUNREACH || ret == -ENOTCONN ?
                GATEWAY_COMMAND_EVENT_REASON_ROUTE_UNAVAILABLE :
                ret == -ETIMEDOUT ?
                    GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED :
                    GATEWAY_COMMAND_EVENT_REASON_RADIO);
#if DEVICE_ROLE == ROLE_GATEWAY
        gateway_survey_begin_cleanup();
#endif
    }

out:
#if DEVICE_ROLE == ROLE_GATEWAY
    gateway_survey_schedule_drive();
#endif
}

static int gateway_route_survey_pair_control(const struct proto_packet *host_packet,
                                             const uint8_t *host_payload,
                                             size_t host_payload_len,
                                             enum command_id command_id)
{
    struct mesh_outbound outbound = {0};
    struct survey_pair pair = {0};
    size_t payload_len = 0u;
    uint64_t absolute_deadline_ms;
    uint32_t delivery_handle = 0u;
    uint32_t client_token;
    uint16_t seq;
    int ret;

    if (host_packet == NULL ||
        host_payload == NULL ||
        host_packet->msg_type != MSG_COMMAND ||
        host_packet->payload_len != host_payload_len ||
        (command_id != CMD_SURVEY_PREPARE_PAIR &&
         command_id != CMD_SURVEY_START_PAIR)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           1u);
        return -EINVAL;
    }
    if (gateway_survey_active
#if DEVICE_ROLE == ROLE_GATEWAY
        || gateway_survey_cleanup_pending()
#endif
    ) {
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_BUSY,
                                         5u);
        return -EBUSY;
    }

    ret = survey_extract_pair_tlvs(host_payload, host_payload_len, &pair);
    if (ret != PROTO_OK ||
        (host_packet->dst_id != pair.initiator_id &&
         host_packet->dst_id != pair.responder_id)) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == PROTO_OK ? COMMAND_DENIED :
                                           COMMAND_MALFORMED_PAYLOAD,
                                           (uint8_t)(ret == PROTO_OK ? 2u : -ret));
        return ret == PROTO_OK ? -EINVAL : mesh_errno_from_proto(ret);
    }
    if (pair.sample_count > SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_DENIED,
                                           4u);
        LOG_WRN("gateway survey pair control rejected: cmd=0x%04x survey=%u samples=%u runtime_max=%u",
                (unsigned int)command_id,
                pair.survey_id,
                pair.sample_count,
                SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT);
        return -EINVAL;
    }

    if (command_id == CMD_SURVEY_START_PAIR) {
        if (host_payload_len > sizeof(outbound.payload)) {
            ret = PROTO_ERR_NO_SPACE;
        } else {
            memcpy(outbound.payload, host_payload, host_payload_len);
            payload_len = host_payload_len;
            ret = PROTO_OK;
        }
    } else {
        ret = survey_append_pair_tlvs(outbound.payload,
                                      sizeof(outbound.payload),
                                      &payload_len,
                                      &pair);
    }
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }

    seq = host_packet->seq == 0u ? gateway_next_command_seq() : host_packet->seq;
    if (command_id == CMD_SURVEY_PREPARE_PAIR) {
        ret = survey_init_pair_prepare_packet(&outbound.packet,
                                              &pair,
                                              DEVICE_ID,
                                              seq,
                                              (uint8_t)payload_len);
    } else {
        outbound.packet.msg_type = MSG_COMMAND;
        outbound.packet.flags = host_packet->flags & FLAG_DIAGNOSTIC;
        outbound.packet.src_id = DEVICE_ID;
        outbound.packet.dst_id = host_packet->dst_id;
        outbound.packet.session_id = pair.survey_id;
        outbound.packet.seq = seq;
        outbound.packet.ttl = host_packet->ttl != 0u ?
                              host_packet->ttl : MESH_DEFAULT_TTL;
        outbound.packet.payload_len = (uint8_t)payload_len;
        ret = PROTO_OK;
    }
    if (ret != PROTO_OK) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           COMMAND_INTERNAL_ERROR,
                                           (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound.packet.dst_id = host_packet->dst_id;
    outbound.payload_len = (uint8_t)payload_len;
    absolute_deadline_ms = (uint64_t)k_uptime_get() +
                           SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS;
    client_token = ((uint32_t)command_id << 16) | seq;

    ret = gateway_begin_command_result_wait_for(
        &outbound.packet,
        command_id,
        SURVEY_PAIR_CONTROL_RESULT_TIMEOUT_MS);
    if (ret < 0) {
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                           (uint8_t)(-ret));
        return ret;
    }

    ret = gateway_survey_send_pair_control(
        &outbound,
        absolute_deadline_ms,
        client_token,
        &delivery_handle);
    if (ret < 0) {
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_host_command_result(host_packet,
                                           command_id,
                                           ret == -EHOSTUNREACH ? COMMAND_TIMEOUT :
                                           ret == -EBUSY ? COMMAND_BUSY :
                                           COMMAND_RADIO_ERROR,
                                           (uint8_t)(-ret));
        return ret;
    }
    ret = app_node_comm_auto_reap_delivery(delivery_handle);
    if (ret < 0) {
        (void)app_node_comm_abandon_delivery(delivery_handle);
        gateway_clear_pending_command_result(&outbound.packet);
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway survey pair control submitted: cmd=0x%04x survey=%u initiator=0x%016llx responder=0x%016llx samples=%u seq=%u handle=%u",
            (unsigned int)command_id,
            pair.survey_id,
            (unsigned long long)pair.initiator_id,
            (unsigned long long)pair.responder_id,
            pair.sample_count,
            seq,
            delivery_handle);
    return 0;
}

static int gateway_route_survey_command(const struct proto_packet *host_packet,
                                        const uint8_t *host_payload,
                                        size_t host_payload_len,
                                        enum command_id command_id)
{
    switch (command_id) {
    case CMD_SURVEY_REACHABILITY:
        return gateway_route_survey_reachability(host_packet, host_payload, host_payload_len);
    case CMD_SURVEY_PREPARE_PAIR:
    case CMD_SURVEY_START_PAIR:
        return gateway_route_survey_pair_control(host_packet,
                                                 host_payload,
                                                 host_payload_len,
                                                 command_id);
    default:
        gateway_emit_host_command_result(host_packet,
                                         command_id,
                                         COMMAND_UNSUPPORTED_COMMAND,
                                         1u);
        return -ENOTSUP;
    }
}

static bool gateway_host_command_send_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -ENOSPC ||
           ret == -EIO || ret == -ETIMEDOUT || ret == -ENOTCONN ||
           ret == -EHOSTUNREACH;
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_route_mesh_host_packet(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len)
{
    struct app_mesh_command_orchestrator *orchestrator;
    struct mesh_outbound outbound = {0};
    struct gateway_command_options command_options;
    enum command_id command_id;
    enum gateway_command_tracking_mode tracking_mode;
    bool sent_now = false;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    if (packet == NULL || payload == NULL || payload_len > sizeof(outbound.payload)) {
        return -EINVAL;
    }
    if (packet->msg_type != MSG_COMMAND) {
        outbound.packet = *packet;
        outbound.packet.payload_len = (uint16_t)payload_len;
        outbound.payload_len = (uint16_t)payload_len;
        memcpy(outbound.payload, payload, payload_len);
        return mesh_start_tracked_tx(&outbound, "ble-host-packet");
    }

    orchestrator = mesh_gateway_command_orchestrator_context();
    ret = app_mesh_command_orchestrator_prepare_flood(
        orchestrator,
        DEVICE_ID,
        k_uptime_get_32(),
        packet->seq == 0u ? gateway_next_command_seq() : 0u);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway rejected BLE host command: %d", ret);
        gateway_emit_host_command_result(packet,
                                         CMD_VENDOR_BASE,
                                         ret == PROTO_ERR_ARG ? COMMAND_DENIED :
                                         COMMAND_MALFORMED_PAYLOAD,
                                         (uint8_t)(-ret));
        return mesh_errno_from_proto(ret);
    }
    outbound = orchestrator->gateway_flow.outbound;
    command_options = orchestrator->gateway_flow.options;
    command_id = orchestrator->gateway_flow.command_id;
    tracking_mode = orchestrator->gateway_flow.tracking_mode;
    if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
        ret = gateway_begin_command_result_wait(&outbound.packet, command_id);
        if (ret < 0) {
            LOG_WRN("gateway command result tracker busy: cmd=0x%04x dst=0x%016llx ret=%d",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id,
                    ret);
            gateway_emit_host_command_result(&outbound.packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
        ret = gateway_begin_command_collection(&command_options);
        if (ret < 0) {
            LOG_WRN("gateway command collection tracker busy: cmd=0x%04x command_seq=%u ret=%d",
                    (unsigned int)command_id,
                    command_options.command_seq,
                    ret);
            gateway_emit_host_command_result(&outbound.packet,
                                             command_id,
                                             ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                             (uint8_t)(-ret));
            return ret;
        }
    }

    if (gateway_command_transport_mode_from_outbound(&outbound) ==
        GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST) {
        ret = mesh_send_gateway_command_flood(orchestrator,
                                              "ble-command-broadcast",
                                              &sent_now);
        if (ret == 0 && sent_now) {
            mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
        }
    } else {
        ret = mesh_start_tracked_tx(&outbound, "ble-command");
    }
    if (ret < 0) {
        LOG_WRN("gateway BLE command route failed: cmd=0x%04x dst=0x%016llx ret=%d",
                (unsigned int)command_id,
                (unsigned long long)outbound.packet.dst_id,
                ret);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            LOG_INF("gateway command waiting for reactive route discovery: cmd=0x%04x dst=0x%016llx",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id);
            return 0;
        }
        if (gateway_host_command_send_retryable(ret)) {
            if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
                gateway_clear_pending_command_result(&outbound.packet);
            } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
                gateway_clear_command_collection(&command_options);
            }
            LOG_INF("gateway command retained for send retry: cmd=0x%04x dst=0x%016llx ret=%d",
                    (unsigned int)command_id,
                    (unsigned long long)outbound.packet.dst_id,
                    ret);
            return -EAGAIN;
        }
        if (tracking_mode == GATEWAY_COMMAND_TRACK_LEGACY_RESULT) {
            gateway_clear_pending_command_result(&outbound.packet);
        } else if (tracking_mode == GATEWAY_COMMAND_TRACK_COLLECTION) {
            gateway_clear_command_collection(&command_options);
        }
        gateway_emit_host_command_result(&outbound.packet,
                                         command_id,
                                         ret == -EBUSY ? COMMAND_BUSY : COMMAND_INVALID_STATE,
                                         (uint8_t)(-ret));
        return ret;
    }

    LOG_INF("gateway BLE command routed: cmd=0x%04x dst=0x%016llx session=%u seq=%u ttl=%u",
            (unsigned int)command_id,
            (unsigned long long)outbound.packet.dst_id,
            outbound.packet.session_id,
            outbound.packet.seq,
            outbound.packet.ttl);
    return 0;
}

static int GATEWAY_BLE_HOST_COMMAND_UNUSED gateway_route_host_packet(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len)
{
    enum command_id command_id = CMD_VENDOR_BASE;
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -EINVAL;
    }

    if (packet != NULL && packet->msg_type == MSG_COMMAND) {
        ret = gateway_command_extract_id(payload, payload_len, &command_id);
        if (ret == PROTO_OK &&
            command_id == CMD_FORCE_REDISCOVERY &&
            packet->dst_id == DEVICE_ID) {
            uint32_t command_budget_ms;
            bool budget_explicit;

            ret = gateway_command_extract_budget_ms(
                payload,
                payload_len,
                APP_NODE_COMM_ROUTE_REFRESH_DEFAULT_TIMEOUT_MS,
                &command_budget_ms,
                &budget_explicit);
            if (ret != PROTO_OK) {
                gateway_emit_host_command_result(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    (uint8_t)(-ret));
                gateway_observe_host_terminal(
                    packet,
                    command_id,
                    COMMAND_MALFORMED_PAYLOAD,
                    GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST);
                return mesh_errno_from_proto(ret);
            }
            ret = app_node_comm_request_route_refresh_correlated_bounded(
                0u,
                "force-rediscovery-ble",
                packet,
                command_budget_ms);
            if (ret < 0) {
                enum command_status status = ret == -EBUSY ?
                    COMMAND_BUSY : COMMAND_INTERNAL_ERROR;
                enum gateway_command_event_reason event_reason =
                    ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                                    GATEWAY_COMMAND_EVENT_REASON_INTERNAL;

                gateway_emit_host_command_result(packet,
                                                 command_id,
                                                 status,
                                                 (uint8_t)event_reason);
                gateway_observe_host_terminal(packet,
                                              command_id,
                                              status,
                                              event_reason);
            }
            return ret;
        }
        if (ret == PROTO_OK &&
            command_id == CMD_ASSIGN_DISCOVERY_SLOTS &&
            packet->dst_id == DEVICE_ID) {
            return gateway_start_discovery_assignment(packet,
                                                      payload,
                                                      payload_len);
        }
        if (ret == PROTO_OK && gateway_command_uses_survey_mesh(command_id)) {
            return gateway_route_survey_command(packet, payload, payload_len, command_id);
        }
        if (ret == PROTO_OK && command_id == CMD_SURVEY_ABORT && gateway_survey_active) {
            LOG_INF("gateway survey orchestration aborted by host: survey=%u",
                    gateway_survey_context.survey_id);
            gateway_survey_auto_finish_status(
                COMMAND_DENIED, GATEWAY_COMMAND_EVENT_REASON_ABORTED);
        }
    }

    return gateway_route_mesh_host_packet(packet, payload, payload_len);
}

#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
static int gateway_host_command_admit(
    void *ctx,
    struct app_gateway_command_ingress_item *item)
{
    int ret;

    ARG_UNUSED(ctx);

    if (item == NULL) {
        return -EINVAL;
    }
    gateway_host_command_next_admission_id++;
    if (gateway_host_command_next_admission_id == 0u) {
        gateway_host_command_next_admission_id = 1u;
    }
    item->admission_id = gateway_host_command_next_admission_id;
    ret = app_gateway_command_lifecycle_admit(&gateway_host_command_lifecycle,
                                               item);
    if (ret < 0) {
        return ret;
    }
    if (k_msgq_num_free_get(&gateway_host_command_msgq) == 0u) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return -ENOSPC;
    }
    ret = gateway_observe_host_acceptance(&item->packet, item->command_id);
    if (ret < 0) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return ret;
    }
    ret = k_msgq_put(&gateway_host_command_msgq, item, K_NO_WAIT);
    if (ret < 0) {
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, item);
        return ret;
    }
    return 0;
}

static int gateway_host_command_submit_priority(void *ctx)
{
    ARG_UNUSED(ctx);

    app_mesh_command_orchestrator_clear_safe_boundary(
        mesh_gateway_command_orchestrator_context());
    return mesh_gateway_command_priority_submit(&gateway_host_command_work);
}

static int gateway_host_command_cancel_admitted(
    void *ctx,
    const struct app_gateway_command_identity *identity)
{
    ARG_UNUSED(ctx);

    return app_gateway_command_lifecycle_cancel(&gateway_host_command_lifecycle,
                                                identity,
                                                NULL,
                                                NULL,
                                                NULL);
}

static void gateway_host_command_emit_result(
    void *ctx,
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason)
{
    enum gateway_command_event_kind kind = gateway_observability_kind(command_id);

    ARG_UNUSED(ctx);

    gateway_emit_host_command_result(command, command_id, status, reason);
    if (kind != 0 && command != NULL) {
        struct gateway_command_event event = gateway_observability_event(
            kind,
            GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
            command_id,
            command,
            0u);

        event.status = status;
        event.reason = status == COMMAND_BUSY ?
                       GATEWAY_COMMAND_EVENT_REASON_BUSY :
                       GATEWAY_COMMAND_EVENT_REASON_INTERNAL;
        (void)gateway_observe_command_event(&event, true);
    }
}

static void gateway_host_command_note_decoded(
    void *ctx,
    const struct app_gateway_command_ingress_item *item)
{
    ARG_UNUSED(ctx);

    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_ble msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         item->packet.msg_type,
                         (unsigned long long)item->packet.src_id,
                         (unsigned long long)item->packet.dst_id,
                         item->packet.seq,
                         (unsigned int)item->payload_len);
}

static void gateway_host_command_schedule_failed(void *ctx, int error)
{
    struct app_gateway_command_ingress_item item;

    ARG_UNUSED(ctx);
    gateway_host_command_retry_pending = false;
    gateway_host_command_retry_round = 0u;
    gateway_host_command_retry_started_ms = 0u;
    while (k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT) == 0) {
        enum app_gateway_command_lifecycle_dispatch dispatch;
        int ret;

        ret = app_gateway_command_lifecycle_begin_dispatch(
            &gateway_host_command_lifecycle, &item, &dispatch);
        if (ret == 0 &&
            dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED) {
            continue;
        }
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-error));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_finish(
            &gateway_host_command_lifecycle, &item);
    }
    LOG_ERR("gateway command priority safe-boundary scheduling failed: ret=%d; admitted commands cancelled",
            error);
}

static void gateway_host_command_retry_work_handler(struct k_work *work)
{
    int ret;

    ARG_UNUSED(work);

    gateway_host_command_retry_pending = false;
    ret = gateway_host_command_submit_priority(NULL);
    if (ret < 0) {
        gateway_host_command_schedule_failed(NULL, ret);
    }
}

static void gateway_host_command_submit_next_queued(void)
{
    struct app_gateway_command_ingress_item item;
    int ret;

    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    ret = gateway_host_command_submit_priority(NULL);
    if (ret < 0) {
        gateway_host_command_schedule_failed(NULL, ret);
    }
}

static void gateway_host_command_work_handler(struct k_work *work)
{
    struct app_gateway_command_ingress_item item;
    enum app_gateway_command_lifecycle_dispatch dispatch;
    int ret;

    ARG_UNUSED(work);
    gateway_host_command_retry_pending = false;

    if (k_msgq_peek(&gateway_host_command_msgq, &item) != 0) {
        return;
    }
    ret = app_gateway_command_lifecycle_begin_dispatch(
        &gateway_host_command_lifecycle, &item, &dispatch);
    if (ret < 0) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_discard(
            &gateway_host_command_lifecycle, &item);
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
        gateway_host_command_submit_next_queued();
        return;
    }
    if (dispatch == APP_GATEWAY_COMMAND_LIFECYCLE_DISPATCH_CANCELLED) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_host_command_retry_round = 0u;
        gateway_host_command_retry_started_ms = 0u;
        gateway_host_command_submit_next_queued();
        return;
    }
    ret = app_mesh_command_orchestrator_activate(
        mesh_gateway_command_orchestrator_context(), &item);
    if (ret < 0) {
        (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
        gateway_emit_host_command_result(&item.packet, item.command_id,
                                         COMMAND_INTERNAL_ERROR,
                                         (uint8_t)(-ret));
        gateway_observe_host_terminal(
            &item.packet,
            item.command_id,
            COMMAND_INTERNAL_ERROR,
            GATEWAY_COMMAND_EVENT_REASON_INTERNAL);
        (void)app_gateway_command_lifecycle_finish(
            &gateway_host_command_lifecycle, &item);
        gateway_host_command_submit_next_queued();
        return;
    }
    dwm3000_driver_clear_receive_abort();
    gateway_observe_host_stage(&item.packet,
                               item.command_id,
                               GATEWAY_COMMAND_EVENT_STAGE_DISPATCHING);
    ret = gateway_route_host_packet(&item.packet, item.payload, item.payload_len);
    if (ret == -EAGAIN) {
        uint32_t delay_ms;

        if (gateway_host_command_retry_round == 0u) {
            gateway_host_command_retry_started_ms = k_uptime_get_32();
        }
        if (gateway_host_command_retry_round >=
            GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS) {
            gateway_emit_host_command_result(&item.packet,
                                             item.command_id,
                                             COMMAND_TIMEOUT,
                                             ETIMEDOUT);
            gateway_observe_host_terminal(
                &item.packet,
                item.command_id,
                COMMAND_TIMEOUT,
                GATEWAY_COMMAND_EVENT_REASON_RETRY_EXHAUSTED);
            LOG_ERR("gateway BLE command send retries exhausted: cmd=0x%04x attempts=%u age_ms=%u",
                    (unsigned int)item.command_id,
                    gateway_host_command_retry_round,
                    k_uptime_get_32() - gateway_host_command_retry_started_ms);
            (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
            (void)app_gateway_command_lifecycle_finish(
                &gateway_host_command_lifecycle, &item);
            gateway_host_command_retry_round = 0u;
            gateway_host_command_retry_started_ms = 0u;
            gateway_host_command_submit_next_queued();
            return;
        }
        delay_ms = discovery_assignment_retry_backoff_ms(
            gateway_host_command_retry_round,
            sys_rand32_get());
        gateway_host_command_retry_round++;
        {
            enum gateway_command_event_kind kind =
                gateway_observability_kind(item.command_id);

            if (kind != 0) {
                struct gateway_command_event event = gateway_observability_event(
                    kind,
                    GATEWAY_COMMAND_EVENT_STAGE_BACKOFF,
                    item.command_id,
                    &item.packet,
                    0u);

                event.attempt = gateway_host_command_retry_round;
                event.status = COMMAND_BUSY;
                event.reason = GATEWAY_COMMAND_EVENT_REASON_BUSY;
                (void)gateway_observe_command_event(&event, false);
            }
        }
        ret = app_gateway_command_lifecycle_requeue_retry(
            &gateway_host_command_lifecycle, &item);
        if (ret < 0) {
            (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
            gateway_emit_host_command_result(&item.packet, item.command_id,
                                             COMMAND_INTERNAL_ERROR,
                                             (uint8_t)(-ret));
            (void)app_gateway_command_lifecycle_finish(
                &gateway_host_command_lifecycle, &item);
            gateway_host_command_retry_round = 0u;
            gateway_host_command_retry_started_ms = 0u;
            gateway_host_command_submit_next_queued();
            return;
        }
        gateway_host_command_retry_pending = true;
        ret = k_work_reschedule(&gateway_host_command_retry_work,
                                K_MSEC(delay_ms));
        if (ret < 0) {
            gateway_host_command_retry_pending = false;
            gateway_host_command_schedule_failed(NULL, ret);
            return;
        }
        LOG_WRN("gateway BLE command send deferred: attempt=%u/%u delay_ms=%u",
                gateway_host_command_retry_round,
                GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS,
                delay_ms);
        return;
    }
    (void)k_msgq_get(&gateway_host_command_msgq, &item, K_NO_WAIT);
    (void)app_gateway_command_lifecycle_finish(&gateway_host_command_lifecycle,
                                                &item);
    gateway_host_command_retry_round = 0u;
    gateway_host_command_retry_started_ms = 0u;
    if (ret < 0) {
        enum gateway_command_event_kind kind =
            gateway_observability_kind(item.command_id);

        /* Reachability owns every immediate terminal outcome at its source. */
        if (kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION ||
            (kind == GATEWAY_COMMAND_EVENT_KIND_ANCHOR_SURVEY &&
             item.command_id != CMD_SURVEY_REACHABILITY)) {
            struct gateway_command_event event = gateway_observability_event(
                kind,
                GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
                item.command_id,
                &item.packet,
                0u);

            event.status = ret == -EBUSY ? COMMAND_BUSY :
                           ret == -ETIMEDOUT || ret == -EHOSTUNREACH ?
                           COMMAND_TIMEOUT :
                           ret == -EINVAL || ret == -EMSGSIZE ?
                           COMMAND_MALFORMED_PAYLOAD : COMMAND_RADIO_ERROR;
            event.reason = item.command_id == CMD_SURVEY_REACHABILITY && ret == -EINVAL ?
                           GATEWAY_COMMAND_EVENT_REASON_SURVEY_RADIO_PREPARATION :
                           ret == -EBUSY ? GATEWAY_COMMAND_EVENT_REASON_BUSY :
                           ret == -ETIMEDOUT || ret == -EHOSTUNREACH ?
                           GATEWAY_COMMAND_EVENT_REASON_TIMEOUT :
                           ret == -EINVAL || ret == -EMSGSIZE ?
                           GATEWAY_COMMAND_EVENT_REASON_INVALID_REQUEST :
                           GATEWAY_COMMAND_EVENT_REASON_RADIO;
            (void)gateway_observe_command_event(&event, true);
        }
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                item.packet.msg_type,
                (unsigned long long)item.packet.dst_id,
                ret);
    }
    gateway_host_command_submit_next_queued();
}
#endif

#if defined(CONFIG_IMEC_GATEWAY_BLE)
void gateway_handle_ble_frame(const uint8_t *frame, size_t frame_len)
{
#if defined(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST)
    int ret;

    ret = gateway_ble_send_packet_frame(frame, frame_len);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(gateway_ble_notify_failures);
        LOG_WRN("gateway BLE connectivity test echo failed: frame_len=%u ret=%d",
                (unsigned int)frame_len,
                ret);
        return;
    }
    LOG_INF("gateway BLE connectivity test echoed packet frame: frame_len=%u",
            (unsigned int)frame_len);
#elif defined(CONFIG_IMEC_ML_CLICKER)
    ml_clicker_handle_ble_frame(frame, frame_len);
#elif defined(CONFIG_IMEC_ML_ANCHOR)
    LOG_WRN("ML anchor BLE packet RX ignored: frame_len=%u",
            (unsigned int)frame_len);
#else
#if DEVICE_ROLE == ROLE_GATEWAY
    struct app_mesh_command_orchestrator *orchestrator =
        mesh_gateway_command_orchestrator_context();
    const struct app_gateway_command_ingress_ops ingress_ops = {
        .gateway_role = true,
        .admit = gateway_host_command_admit,
        .submit_priority = gateway_host_command_submit_priority,
        .cancel_admitted = gateway_host_command_cancel_admitted,
        .emit_result = gateway_host_command_emit_result,
        .note_decoded = gateway_host_command_note_decoded,
    };
    bool command_handled;
    int ret;

    ret = app_mesh_command_orchestrator_gateway_ingress(
        orchestrator,
        &ingress_ops,
        frame,
        frame_len,
        &command_handled);
    if (ret != 0) {
        if (command_handled) {
            LOG_WRN("gateway BLE command ingress failed: %d", ret);
        } else {
            LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        }
        return;
    }
    if (command_handled) {
        return;
    }

    /* Ingress always decodes into the shared context, including non-commands. */
    ret = gateway_route_host_packet(&orchestrator->admitted.packet,
                                    orchestrator->admitted.payload,
                                    orchestrator->admitted.payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                orchestrator->admitted.packet.msg_type,
                (unsigned long long)orchestrator->admitted.packet.dst_id,
                ret);
    }
#else
    struct proto_packet packet = {0};
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    int ret;

    ret = serial_frame_decode_packet(frame,
                                     frame_len,
                                     &packet,
                                     payload,
                                     sizeof(payload),
                                     &payload_len);
    if (ret != PROTO_OK) {
        LOG_WRN("gateway BLE COBS frame decode failed: %d", ret);
        return;
    }
    HIGH_DEBUG_COUNTER_INC(command_rx);
    high_debug_log_event("COMMAND_RX",
                         "transport=gateway_ble msg=0x%02x src=0x%016llx dst=0x%016llx seq=%u payload_len=%u",
                         packet.msg_type,
                         (unsigned long long)packet.src_id,
                         (unsigned long long)packet.dst_id,
                         packet.seq,
                         (unsigned int)payload_len);

    ret = gateway_route_host_packet(&packet, payload, payload_len);
    if (ret < 0) {
        LOG_WRN("gateway BLE packet rejected: msg=0x%02x dst=0x%016llx ret=%d",
                packet.msg_type,
                (unsigned long long)packet.dst_id,
                ret);
    }
#endif
#endif
}
#endif

static void anchor_uwb_scan_schedule_ms(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    int ret = k_work_reschedule_for_queue(&anchor_uwb_scan_work_q,
                                          &anchor_uwb_scan_work,
                                          K_MSEC(delay_ms));

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_RESCHED ret=%d delay=%u\n",
                            ret,
                            delay_ms);
    }
#else
    ARG_UNUSED(delay_ms);
#endif
}

static uint32_t anchor_uwb_scan_blocked_retry_ms(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return ANCHOR_UWB_SCAN_BUSY_RETRY_MS;
    }
    return anchor_uwb_scan_interval_ms;
}

static int anchor_queue_survey_sample_result(
    const struct survey_pair *pair,
    uint16_t sample_index,
    uint64_t reporter_id,
    const struct dwm3000_range_result *range_result)
{
    struct mesh_outbound outbound = {0};
    struct survey_sample sample = {0};
    size_t payload_len = 0u;
    enum range_status status;
    int64_t sample_local_ms;
    int ret;

    if (pair == NULL || reporter_id == 0u || range_result == NULL) {
        return -EINVAL;
    }

    status = range_status_valid(range_result->status) ?
             range_result->status : RANGE_INTERNAL_ERROR;

    sample.pair = *pair;
    sample.sample_index = sample_index;
    sample.distance_mm = range_result->distance_mm;
    sample.quality = range_result->quality;
    sample.range_status = status;

    ret = survey_append_sample_tlvs(outbound.payload,
                                    sizeof(outbound.payload),
                                    &payload_len,
                                    &sample);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    sample_local_ms = range_result->exchange_started ?
                      range_result->exchange_start_ms :
                      k_uptime_get();
    ret = anchor_append_sequence_time_tlvs(outbound.payload,
                                           sizeof(outbound.payload),
                                           &payload_len,
                                           sample_local_ms);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = append_range_result_timing_tlvs(outbound.payload,
                                          sizeof(outbound.payload),
                                          &payload_len,
                                          range_result);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = survey_init_result_packet_from_reporter(
        &outbound.packet,
        &sample,
        reporter_id,
        GATEWAY_ID,
        app_anchor_survey_runtime_next_sequence(),
        (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    outbound.payload_len = (uint8_t)payload_len;
    return queue_anchor_report(&outbound);
}

static bool anchor_relay_tx_active(void)
{
    return mesh_relay_tx_active(&mesh_runtime);
}

static void anchor_range_window_record(
    struct anchor_range_window_report *report,
    const struct dwm3000_range_result *result)
{
    int8_t sampled_rsl = 0;
    uint8_t sampled_cir[UWB_CIR_SAMPLE_LEN] = {0};
    uint16_t sampled_full_cir_len = 0u;
    uint16_t sampled_full_cir_total_len = 0u;
    uint16_t sampled_full_cir_first_path_index = 0u;
    uint16_t sampled_full_cir_start_index = 0u;
    bool sampled_full_cir_truncated = false;

    if (report == NULL || result == NULL) {
        return;
    }

    if (report->rsl_sampled) {
        sampled_rsl = report->result.rsl_dbm;
    }
    if (report->cir_sampled) {
        memcpy(sampled_cir, report->result.cir_sample, UWB_CIR_SAMPLE_LEN);
    }
    if (report->anchor_full_cir_sampled) {
        sampled_full_cir_len = report->result.anchor_full_cir_len;
        sampled_full_cir_total_len = report->result.anchor_full_cir_total_len;
        sampled_full_cir_first_path_index =
            report->result.anchor_full_cir_first_path_index;
        sampled_full_cir_start_index = report->result.anchor_full_cir_start_index;
        sampled_full_cir_truncated = report->result.anchor_full_cir_truncated;
    }
    if (!report->have_exchange_start_ms && result->exchange_started) {
        report->first_exchange_start_ms = result->exchange_start_ms;
        report->have_exchange_start_ms = true;
    }

    if (!report->have_result || result->status == RANGE_OK) {
        report->result = *result;
        report->have_result = true;
    }
    if (report->have_exchange_start_ms) {
        report->result.exchange_start_ms = report->first_exchange_start_ms;
        report->result.exchange_started = true;
    }
    if (result->rsl_sampled && !report->rsl_sampled) {
        sampled_rsl = result->rsl_dbm;
        report->rsl_sampled = true;
    }
    if (report->rsl_sampled) {
        report->result.rsl_dbm = sampled_rsl;
        report->result.rsl_sampled = true;
    }
    if (result->cir_sampled && !report->cir_sampled) {
        memcpy(sampled_cir, result->cir_sample, UWB_CIR_SAMPLE_LEN);
        report->cir_sampled = true;
    }
    if (report->cir_sampled) {
        memcpy(report->result.cir_sample, sampled_cir, UWB_CIR_SAMPLE_LEN);
        report->result.cir_sampled = true;
    }
    if (result->anchor_full_cir_sampled && !report->anchor_full_cir_sampled) {
        sampled_full_cir_len = result->anchor_full_cir_len;
        sampled_full_cir_total_len = result->anchor_full_cir_total_len;
        sampled_full_cir_first_path_index = result->anchor_full_cir_first_path_index;
        sampled_full_cir_start_index = result->anchor_full_cir_start_index;
        sampled_full_cir_truncated = result->anchor_full_cir_truncated;
        report->anchor_full_cir_sampled = true;
    }
    if (report->anchor_full_cir_sampled) {
        report->result.anchor_full_cir_len = sampled_full_cir_len;
        report->result.anchor_full_cir_total_len = sampled_full_cir_total_len;
        report->result.anchor_full_cir_first_path_index =
            sampled_full_cir_first_path_index;
        report->result.anchor_full_cir_start_index = sampled_full_cir_start_index;
        report->result.anchor_full_cir_truncated = sampled_full_cir_truncated;
        report->result.anchor_full_cir_sampled = true;
    }

    if (result->status != RANGE_OK ||
        report->sample_count >= RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return;
    }

    report->distance_samples_mm[report->sample_count] = result->distance_mm;
    report->sample_sequence_start_ms[report->sample_count] =
        result->exchange_started ? result->exchange_start_ms : k_uptime_get();
    report->range_round_indices[report->sample_count] = result->round_index;
    report->distance_sum_mm += result->distance_mm;
    report->quality_sum += result->quality;
    report->sample_count++;
}

static void anchor_range_window_finalize(
    struct anchor_range_window_report *report)
{
    if (report == NULL || !report->have_result || report->sample_count == 0u) {
        return;
    }

    report->result.distance_mm = average_i32_nearest(report->distance_sum_mm,
                                                     report->sample_count);
    report->result.quality = (uint8_t)((report->quality_sum +
                                        (report->sample_count / 2u)) /
                                       report->sample_count);
    report->result.status = RANGE_OK;
}

static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);

static void anchor_set_uwb_busy(bool busy)
{
    k_spinlock_key_t key = k_spin_lock(&anchor_uwb_lock);

    anchor_uwb_busy = busy;
    k_spin_unlock(&anchor_uwb_lock, key);
}

static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us)
{
    int64_t elapsed_ms;
    uint64_t elapsed_us;

    if (DEVICE_ROLE != ROLE_ANCHOR || start_ms < 0) {
        return;
    }

    elapsed_ms = k_uptime_get() - start_ms;
    if (elapsed_ms <= 0) {
        return;
    }

    elapsed_us = (uint64_t)elapsed_ms * 1000u;
    if (elapsed_us <= already_counted_us) {
        return;
    }
    elapsed_us -= already_counted_us;
    uwb_anchor_note_awake_time(&anchor_uwb_session,
                               (uint32_t)MIN(elapsed_us, (uint64_t)UINT32_MAX));
}

static void anchor_hold_channel5_rx_until_ms(int64_t deadline_ms,
                                             bool *deferred_mesh_rx_queued)
{
    uint32_t frame_count = 0u;
    uint32_t error_count = 0u;

    while (k_uptime_get() < deadline_ms) {
        struct uwb_wake_claim_frame repeated_claim;
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        int64_t remaining_ms = deadline_ms - k_uptime_get();
        int ret;

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      anchor_uwb_scan_frame,
                                                      sizeof(anchor_uwb_scan_frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            error_count++;
            continue;
        }
        frame_count++;

        if (uwb_decode_wake_claim(anchor_uwb_scan_frame,
                                  frame_len,
                                  &repeated_claim) == PROTO_OK) {
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(anchor_uwb_scan_frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued && deferred_mesh_rx_queued != NULL) {
                *deferred_mesh_rx_queued = true;
            }
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_HOLD_DONE deadline=%lld now=%u frames=%u errors=%u deferred=%u\n",
                            (long long)deadline_ms,
                            k_uptime_get_32(),
                            frame_count,
                            error_count,
                            (deferred_mesh_rx_queued != NULL &&
                             *deferred_mesh_rx_queued) ? 1u : 0u);
    }
}

static uint32_t anchor_run_scheduled_uwb_ranges(const struct uwb_range_schedule_frame *schedule,
                                                int64_t schedule_rx_ms,
                                                bool *deferred_mesh_rx_queued)
{
    struct anchor_range_window_report window_report = {0};
    uint8_t *anchor_full_cir = NULL;
    size_t anchor_full_cir_cap = 0u;
    uint32_t retained_sleep_us = 0u;
    size_t total_samples;
    int64_t no_poll_deadline_ms;
    bool poll_received = false;
    bool poll_guard_expired = false;
    bool last_local_exchange_ok = false;

    if (schedule == NULL) {
        return 0u;
    }

    if ((schedule->flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) != 0u) {
        anchor_full_cir = mesh_anchor_click_cir_capture_begin(&anchor_full_cir_cap);
    }

    total_samples = uwb_range_schedule_total_samples(schedule);
    no_poll_deadline_ms = k_uptime_get() + ANCHOR_SCHEDULE_NO_POLL_GUARD_MS;
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_enter_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    LOG_INF("anchor scheduled UWB ranging start: clicker=0x%016llx event_seq=%u selected=%u total_samples=%u",
            (unsigned long long)schedule->clicker_id,
            schedule->click_event_id,
            schedule->selected_count,
            (unsigned int)total_samples);
    app_anchor_log_range_schedule("anchor", schedule);

    for (size_t sample_index = 0u; sample_index < total_samples; sample_index++) {
        struct dwm3000_range_request expected;
        struct dwm3000_range_result range_result;
        struct uwb_range_exchange_identity identity;
        uint64_t anchor_id = 0u;
        uint8_t seq = 0u;
        uint8_t round_index = 0u;
        int64_t listen_start_ms;
        int64_t listen_deadline_ms;
        int ret = -ETIMEDOUT;

        ret = uwb_range_schedule_sample_at(schedule, sample_index, &anchor_id, &seq);
        if (ret != PROTO_OK || anchor_id != DEVICE_ID) {
            high_debug_log_event("DS_TWR_POLL_RX",
                                 "sample=%u/%u expected_anchor=0x%016llx local_anchor=0x%016llx result=wrong-target",
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 (unsigned long long)anchor_id,
                                 (unsigned long long)DEVICE_ID);
            continue;
        }

        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        memset(&identity, 0, sizeof(identity));
        identity.network_id = schedule->network_id;
        identity.clicker_id = schedule->clicker_id;
        identity.click_event_id = schedule->click_event_id;
        identity.attempt_index = schedule->attempt_index;
        identity.nonce = schedule->nonce;
        identity.anchor_id = anchor_id;
        identity.ranging_channel = schedule->ranging_channel;
        identity.reply_delay_us = schedule->reply_delay_us;
        identity.seq = seq;
        identity.flags = schedule->flags;
        ret = uwb_anchor_range_round_index(&anchor_uwb_session,
                                           &identity,
                                           &round_index);
        if (ret != PROTO_OK) {
            uwb_anchor_note_timing_rejection(&anchor_uwb_session);
            continue;
        }
        uwb_anchor_note_sample_order(&anchor_uwb_session);

        listen_start_ms =
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index) / 1000;
        listen_start_ms -= UWB_SCHEDULE_GUARD_MS;
        if (!poll_received && listen_start_ms > no_poll_deadline_ms) {
            anchor_hold_channel5_rx_until_ms(no_poll_deadline_ms,
                                             deferred_mesh_rx_queued);
            poll_guard_expired = true;
            break;
        }
        anchor_hold_channel5_rx_until_ms(listen_start_ms,
                                         deferred_mesh_rx_queued);

        listen_deadline_ms = ceil_us_to_ms(
            scheduled_range_sample_target_us(schedule_rx_ms, schedule, sample_index + 1u));
        listen_deadline_ms += UWB_SCHEDULE_GUARD_MS;

        memset(&expected, 0, sizeof(expected));
        memset(&range_result, 0, sizeof(range_result));
        range_result.status = RANGE_RX_TIMEOUT;
        expected.initiator_id = schedule->clicker_id;
        expected.responder_id = DEVICE_ID;
        expected.network_id = schedule->network_id;
        expected.session_nonce = schedule->nonce;
        expected.responder_short_addr = local_uwb_short_addr();
        expected.session_id = schedule->click_event_id;
        expected.seq = seq;
        expected.round_index = round_index;
        expected.flags = schedule->flags;
        expected.capture_rsl = true;
        expected.skip_responder_report = false;
        expected.expect_clicker_diag = false;
        expected.send_anchor_diag = false;

        LOG_INF("anchor scheduled UWB sample listen: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                (unsigned int)(sample_index + 1u),
                (unsigned int)total_samples,
                round_index,
                seq);
        stage1_led_phase(STAGE1_LED_PHASE_RANGE);
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
        high_debug_log_event("DS_TWR_POLL_RX",
                             "listen=1 clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u schedule_rx_ms=%lld target_us=%lld listen_start_ms=%lld now_ms=%lld deadline_ms=%lld first_poll_ms=%u stride_us=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             uwb_schedule_burst_id(schedule->click_event_id,
                                                   schedule->attempt_index),
                             (unsigned int)(sample_index + 1u),
                             (unsigned int)total_samples,
                             round_index,
                             seq,
                             (long long)schedule_rx_ms,
                             (long long)scheduled_range_sample_target_us(schedule_rx_ms,
                                                                         schedule,
                                                                         sample_index),
                             (long long)listen_start_ms,
                             (long long)k_uptime_get(),
                             (long long)listen_deadline_ms,
                             schedule->first_poll_delay_ms,
                             schedule->exchange_stride_us);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DS_SAMPLE_ARM evt=%u attempt=%u sample=%u/%u round=%u seq=%u target_us=%lld start=%lld now=%u deadline=%lld\n",
                                schedule->click_event_id,
                                schedule->attempt_index,
                                (unsigned int)(sample_index + 1u),
                                (unsigned int)total_samples,
                                round_index,
                                seq,
                                (long long)scheduled_range_sample_target_us(
                                    schedule_rx_ms, schedule, sample_index),
                                (long long)listen_start_ms,
                                k_uptime_get_32(),
                                (long long)listen_deadline_ms);
        }
        while (k_uptime_get() < listen_deadline_ms) {
            int64_t bounded_deadline_ms = listen_deadline_ms;
            int64_t remaining_ms;

            if (!poll_received && bounded_deadline_ms > no_poll_deadline_ms) {
                bounded_deadline_ms = no_poll_deadline_ms;
            }
            remaining_ms = bounded_deadline_ms - k_uptime_get();
            if (remaining_ms <= 0) {
                break;
            }

            expected.timeout_ms = (uint32_t)MAX(1, remaining_ms);
            ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                         &expected,
                                                         expected.timeout_ms,
                                                         &range_result);
            if (ret == -EAGAIN) {
                if (range_result.status == RANGE_WRONG_TARGET) {
                    high_debug_log_event("DS_TWR_POLL_RX",
                                         "result=wrong-target expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                                         (unsigned long long)schedule->clicker_id,
                                         (unsigned long long)range_result.initiator_id,
                                         (unsigned long long)DEVICE_ID,
                                         (unsigned long long)range_result.responder_id,
                                         schedule->click_event_id,
                                         range_result.session_id,
                                         round_index,
                                         range_result.round_index,
                                         seq,
                                         range_result.seq);
                    LOG_DBG("anchor ignored nonmatching UWB POLL: expected_clicker=0x%016llx observed_clicker=0x%016llx expected_anchor=0x%016llx observed_anchor=0x%016llx event_seq=%u observed_event_seq=%u expected_round=%u observed_round=%u expected_seq=%u observed_seq=%u",
                            (unsigned long long)schedule->clicker_id,
                            (unsigned long long)range_result.initiator_id,
                            (unsigned long long)DEVICE_ID,
                            (unsigned long long)range_result.responder_id,
                            schedule->click_event_id,
                            range_result.session_id,
                            round_index,
                            range_result.round_index,
                            seq,
                            range_result.seq);
                } else if (range_result.status == RANGE_BAD_FRAME ||
                           range_result.status == RANGE_RX_ERROR) {
                    LOG_DBG("anchor ignored pre-POLL UWB frame inside scheduled slot: status=%s(%u) expected_clicker=0x%016llx event_seq=%u seq=%u",
                            range_status_name(range_result.status),
                            range_result.status,
                            (unsigned long long)schedule->clicker_id,
                            schedule->click_event_id,
                            seq);
                }
                continue;
            }
            break;
        }
        if (range_result.exchange_started) {
            poll_received = true;
        }
        last_local_exchange_ok = ret == 0 &&
                                 range_result.exchange_started &&
                                 range_result.status == RANGE_OK;
        if (!poll_received && k_uptime_get() >= no_poll_deadline_ms) {
            poll_guard_expired = true;
            break;
        }

        if (!range_result.exchange_started) {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_RX_TIMEOUT;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
            LOG_WRN("anchor scheduled UWB sample did not start: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret);
            high_debug_log_event("RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s exchange_started=0",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status));
        } else if (ret == 0 && range_result.status == RANGE_OK) {
            HIGH_DEBUG_COUNTER_INC(ds_twr_successes);
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, RANGE_OK);
            anchor_range_window_record(&window_report, &range_result);
            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor scheduled UWB sample complete: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u distance_mm=%d quality=%u count=%u",
                    (unsigned long long)range_result.initiator_id,
                    range_result.session_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    range_result.seq,
                    range_result.distance_mm,
                    range_result.quality,
                    window_report.sample_count);
            high_debug_log_event("RANGE_OK",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u anchor=0x%016llx distance_mm=%d quality=%u",
                                 (unsigned long long)range_result.initiator_id,
                                 range_result.session_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 range_result.seq,
                                 (unsigned long long)range_result.responder_id,
                                 range_result.distance_mm,
                                 range_result.quality);
        } else {
            if (range_result.status == RANGE_OK || !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            HIGH_DEBUG_COUNTER_INC(ds_twr_failures);
            if (range_result.status == RANGE_TIMING_INVALID) {
                HIGH_DEBUG_COUNTER_INC(ds_twr_timing_rejects);
            }
            (void)uwb_anchor_note_range_result(&anchor_uwb_session, range_result.status);
            if (range_result.initiator_id != 0u && range_result.responder_id != 0u) {
                anchor_range_window_record(&window_report, &range_result);
            }
            stage1_led_result(range_result.status == RANGE_RX_TIMEOUT ?
                              STAGE1_LED_RESULT_TIMEOUT :
                              STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor scheduled UWB sample failed: clicker=0x%016llx event_seq=%u sample=%u/%u round=%u seq=%u ret=%d status=%s(%u)",
                    (unsigned long long)schedule->clicker_id,
                    schedule->click_event_id,
                    (unsigned int)(sample_index + 1u),
                    (unsigned int)total_samples,
                    round_index,
                    seq,
                    ret,
                    range_status_name(range_result.status),
                    range_result.status);
            high_debug_log_event(range_result.status == RANGE_TIMING_INVALID ?
                                 "RANGE_TIMING_REJECT" : "RANGE_FAIL",
                                 "clicker=0x%016llx event_seq=%u attempt=%u burst_id=%u sample=%u/%u round=%u seq=%u ret=%d reason=%s status=%u",
                                 (unsigned long long)schedule->clicker_id,
                                 schedule->click_event_id,
                                 schedule->attempt_index,
                                 uwb_schedule_burst_id(schedule->click_event_id,
                                                       schedule->attempt_index),
                                 (unsigned int)(sample_index + 1u),
                                 (unsigned int)total_samples,
                                 round_index,
                                 seq,
                                 ret,
                                 range_status_name(range_result.status),
                                 range_result.status);
        }
    }

    if (last_local_exchange_ok && window_report.have_result &&
        anchor_full_cir != NULL && anchor_full_cir_cap > 0u) {
        int cir_ret = dwm3000_driver_capture_last_rx_cir(
            anchor_full_cir,
            (uint16_t)anchor_full_cir_cap,
            &window_report.result);

        window_report.anchor_full_cir_sampled =
            window_report.result.anchor_full_cir_sampled;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DS_BURST stage=cir-after-ranges ret=%d evt=%u seq=%u round=%u bytes=%u\n",
                                cir_ret,
                                schedule->click_event_id,
                                window_report.result.seq,
                                window_report.result.round_index,
                                window_report.result.anchor_full_cir_len);
        }
    }

    if (poll_guard_expired) {
        LOG_WRN("anchor scheduled UWB poll guard expired: clicker=0x%016llx event_seq=%u waited_ms=%u total_samples=%u",
                (unsigned long long)schedule->clicker_id,
                schedule->click_event_id,
                ANCHOR_SCHEDULE_NO_POLL_GUARD_MS,
                (unsigned int)total_samples);
        high_debug_log_event("RANGE_FAIL",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=no-poll guard_ms=%u",
                             (unsigned long long)schedule->clicker_id,
                             schedule->click_event_id,
                             schedule->attempt_index,
                             ANCHOR_SCHEDULE_NO_POLL_GUARD_MS);
    } else if (poll_received &&
               schedule->diagnostics_required != UWB_RANGE_SCHEDULE_DIAGNOSTICS_OMITTED) {
#if defined(CONFIG_IMEC_ML_ANCHOR)
        anchor_scan_recovery_gap_requested = true;
        retained_sleep_us = u32_saturating_add(
            retained_sleep_us,
            ml_anchor_run_post_burst_diagnostics(schedule, schedule_rx_ms, total_samples));
#endif
    }

    anchor_range_window_finalize(&window_report);
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    build_uwb_schedule_report_if_relevant(&anchor_uwb_session,
                                          schedule->flags,
                                          &window_report);
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR) && defined(CONFIG_IMEC_GATEWAY_BLE)
    gateway_ble_exit_uwb_quiet("ml-anchor-scheduled-uwb");
#endif
    return retained_sleep_us;
}

#if defined(CONFIG_IMEC_ML_ANCHOR)
static bool anchor_pair_schedule_matches_epoch(
    const struct uwb_anchor_pair_schedule_frame *schedule)
{
    return schedule != NULL &&
           anchor_uwb_session.epoch.active &&
           schedule->network_id == anchor_uwb_session.epoch.network_id &&
           schedule->clicker_id == anchor_uwb_session.epoch.clicker_id &&
           schedule->survey_id == anchor_uwb_session.epoch.click_event_id &&
           schedule->attempt_index == anchor_uwb_session.epoch.attempt_index &&
           schedule->nonce == anchor_uwb_session.epoch.nonce &&
           schedule->flags == anchor_uwb_session.epoch.flags;
}

static int anchor_send_pair_survey_result(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    uint8_t pair_index,
    uint64_t initiator_id,
    uint64_t responder_id,
    const struct dwm3000_range_result *range_result)
{
    struct uwb_anchor_pair_result_frame result = {0};
    uint8_t frame[UWB_ANCHOR_PAIR_RESULT_LEN];
    size_t frame_len = 0u;
    int ret;

    if (schedule == NULL || range_result == NULL) {
        return -EINVAL;
    }

    result.network_id = schedule->network_id;
    result.clicker_id = schedule->clicker_id;
    result.survey_id = schedule->survey_id;
    result.nonce = schedule->nonce;
    result.initiator_id = initiator_id;
    result.responder_id = responder_id;
    result.pair_index = pair_index;
    result.pair_count = schedule->pair_count;
    result.seq = (uint8_t)(pair_index + 1u);
    result.status = range_status_valid(range_result->status) ?
                    range_result->status : RANGE_INTERNAL_ERROR;
    result.quality = range_result->quality;
    result.distance_mm = range_result->status == RANGE_OK ?
                         range_result->distance_mm : 0;
    result.rsl_dbm = range_result->rsl_sampled ? range_result->rsl_dbm : 0;
    result.flags = schedule->flags;

    ret = uwb_encode_anchor_pair_result(&result, frame, sizeof(frame), &frame_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_CONTROL_TX_TIMEOUT_MS);
    if (ret < 0) {
        LOG_WRN("anchor pair-survey result TX failed: survey=%u pair=%u ret=%d",
                schedule->survey_id,
                pair_index,
                ret);
        return ret;
    }
    LOG_INF("anchor pair-survey result TX: survey=%u pair=%u/%u initiator=0x%016llx responder=0x%016llx status=%s(%u) distance_mm=%d",
            schedule->survey_id,
            (unsigned int)(pair_index + 1u),
            schedule->pair_count,
            (unsigned long long)initiator_id,
            (unsigned long long)responder_id,
            range_status_name(result.status),
            result.status,
            result.distance_mm);
    return 0;
}

static uint32_t anchor_run_clicker_pair_survey(
    const struct uwb_anchor_pair_schedule_frame *schedule,
    int64_t schedule_rx_ms)
{
    uint32_t retained_sleep_us = 0u;
    uint16_t local_start_delay_ms = 0u;
    bool local_listed = false;

    if (!anchor_pair_schedule_matches_epoch(schedule)) {
        LOG_WRN("anchor rejected pair-survey schedule identity");
        return 0u;
    }

    for (uint8_t i = 0u; i < schedule->anchor_count; i++) {
        if (schedule->anchor_ids[i] == DEVICE_ID) {
            local_start_delay_ms = schedule->anchor_start_delay_ms[i];
            local_listed = true;
            break;
        }
    }
    if (!local_listed) {
        LOG_INF("anchor pair-survey schedule not for local anchor: survey=%u",
                schedule->survey_id);
        return 0u;
    }

    LOG_INF("anchor pair-survey start: survey=%u anchors=%u pairs=%u schedule_start_ms=%u stride_ms=%u window_ms=%u rx_guard_ms=%u",
            schedule->survey_id,
            schedule->anchor_count,
            schedule->pair_count,
            local_start_delay_ms,
            schedule->pair_stride_ms,
            schedule->pair_window_ms,
            UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS);

    for (uint8_t pair_index = 0u; pair_index < schedule->pair_count; pair_index++) {
        struct dwm3000_range_request request = {0};
        struct dwm3000_range_result range_result = {0};
        uint64_t initiator_id = 0u;
        uint64_t responder_id = 0u;
        int64_t target_ms;
        int low_power_ret = 0;
        int ret;

        ret = uwb_anchor_pair_at(schedule, pair_index, &initiator_id, &responder_id);
        if (ret != PROTO_OK) {
            continue;
        }
        if (DEVICE_ID != initiator_id && DEVICE_ID != responder_id) {
            continue;
        }

        target_ms = schedule_rx_ms + local_start_delay_ms +
                    ((int64_t)pair_index * schedule->pair_stride_ms);
        request.initiator_id = initiator_id;
        request.responder_id = responder_id;
        request.network_id = schedule->network_id;
        request.session_nonce = schedule->nonce;
        request.responder_short_addr = uwb_session_short_addr_from_id(responder_id);
        request.session_id = schedule->survey_id;
        request.seq = (uint8_t)(pair_index + 1u);
        request.round_index = 0u;
        request.flags = schedule->flags;
        request.timeout_ms = schedule->pair_window_ms;
        request.reply_delay_uus = 0u;
        request.capture_rsl = false;
        request.skip_responder_report = false;

        if (DEVICE_ID == initiator_id) {
            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(target_ms));
            memset(&range_result, 0, sizeof(range_result));
            range_result.status = RANGE_RX_TIMEOUT;
            ret = dwm3000_driver_range_initiator(&request, &range_result);
            low_power_ret = anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,
                                                   "pair-survey-initiator");
            if (ret < 0 && !range_status_valid(range_result.status)) {
                range_result.status = RANGE_INTERNAL_ERROR;
            }
            (void)anchor_send_pair_survey_result(schedule,
                                                 pair_index,
                                                 initiator_id,
                                                 responder_id,
                                                 &range_result);
        } else {
            int64_t listen_start_ms = target_ms -
                                      UWB_ANCHOR_PAIR_SURVEY_RX_EARLY_GUARD_MS;
            int64_t listen_deadline_ms = target_ms + schedule->pair_window_ms;

            retained_sleep_us = u32_saturating_add(
                retained_sleep_us,
                sleep_with_uwb_idle_until_ms(listen_start_ms));
            while (k_uptime_get() < listen_deadline_ms) {
                int64_t remaining_ms = listen_deadline_ms - k_uptime_get();

                memset(&range_result, 0, sizeof(range_result));
                range_result.status = RANGE_RX_TIMEOUT;
                request.timeout_ms = (uint32_t)MAX(1, remaining_ms);
                ret = dwm3000_driver_responder_poll_expected(DEVICE_ID,
                                                             &request,
                                                             request.timeout_ms,
                                                             &range_result);
                if (ret == -EAGAIN) {
                    continue;
                }
                break;
            }
            low_power_ret = anchor_enter_low_power(APP_RADIO_LOW_POWER_IDLE,
                                                   "pair-survey-responder");
        }
        if (low_power_ret < 0) {
            break;
        }
    }

    LOG_INF("anchor pair-survey done: survey=%u", schedule->survey_id);
    return retained_sleep_us;
}
#endif

static bool anchor_handle_uwb_claim(const struct uwb_wake_claim_frame *first_claim,
                                    uint8_t first_quality,
                                    int64_t first_rx_ms,
                                    uint32_t *retained_sleep_us,
                                    bool *deferred_mesh_rx_queued)
{
    struct uwb_wake_claim_frame selected_claim;
    int64_t selected_rx_ms;
    int64_t collect_deadline_ms;
    uint8_t selected_quality = first_quality;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len = 0u;
    int ret;
    int last_discovery_ret = -ETIMEDOUT;
    int last_discovery_decode_ret = PROTO_OK;
    int64_t discovery_start_ms;
    int64_t discovery_deadline_ms;
    enum uwb_anchor_claim_decision decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    enum uwb_anchor_claim_decision selected_decision = UWB_ANCHOR_CLAIM_ACCEPTED;
    bool discover_received = false;
    bool schedule_frame_ready = false;
    int64_t schedule_rx_ms = -1;
    struct uwb_discover_frame discover = {0};
    bool discovery_uses_extended_phr;
    bool click_priority = false;
    uint32_t discovery_late_guard_ms;

    if (first_claim == NULL) {
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        !app_mesh_c5_wake_claim_requires_anchor_handoff(first_claim->flags,
                                                         true)) {
        status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_REJECT_CLICK_PATH src=0x%llx evt=%u attempt=%u flags=0x%02x\n",
                            (unsigned long long)first_claim->clicker_id,
                            first_claim->click_event_id,
                            first_claim->attempt_index,
                            first_claim->flags);
        return false;
    }
    if (retained_sleep_us != NULL) {
        *retained_sleep_us = 0u;
    }

    selected_claim = *first_claim;
    selected_rx_ms = first_rx_ms;
    HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    high_debug_log_event("WAKE_CLAIM_RX",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx nonce=0x%016llx quality=%u",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         (unsigned long long)first_claim->nonce,
                         first_quality);
    ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                       first_claim,
                                       (uint32_t)first_rx_ms,
                                       &decision);
    if (ret != PROTO_OK) {
        HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        high_debug_log_event("WAKE_CLAIM_REJECT",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                             (unsigned long long)first_claim->clicker_id,
                             first_claim->click_event_id,
                             first_claim->attempt_index,
                             claim_decision_name(decision),
                             ret);
        LOG_DBG("anchor rejected UWB WAKE_CLAIM: clicker=0x%016llx ret=%d decision=%u",
                (unsigned long long)first_claim->clicker_id,
                ret,
                decision);
        return false;
    }
    selected_decision = decision;
    HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
    stage1_led_result(STAGE1_LED_RESULT_OK);
    high_debug_log_event("WAKE_CLAIM_ACCEPT",
                         "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                         (unsigned long long)first_claim->clicker_id,
                         first_claim->click_event_id,
                         first_claim->attempt_index,
                         (unsigned long long)first_claim->priority_id,
                         claim_decision_name(decision));
    LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
            (unsigned long long)first_claim->clicker_id,
            first_claim->click_event_id,
            first_claim->attempt_index,
            (unsigned long long)first_claim->priority_id,
            ret,
            decision);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_WAKE_ACCEPT clicker=0x%llx evt=%u attempt=%u flags=0x%02x disc_in=%u q=%u\n",
                            (unsigned long long)first_claim->clicker_id,
                            first_claim->click_event_id,
                            first_claim->attempt_index,
                            first_claim->flags,
                            first_claim->discovery_starts_in_ms,
                            first_quality);
    }
    collect_deadline_ms = k_uptime_get() + ANCHOR_CLAIM_COLLECTION_MS;
    while (k_uptime_get() < collect_deadline_ms) {
        struct uwb_wake_claim_frame claim;
        uint8_t quality = 0u;
        int64_t remaining_ms = collect_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      NULL);
        if (ret == -ETIMEDOUT) {
            break;
        }
        if (ret < 0) {
            continue;
        }
        ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (ret != PROTO_OK) {
            enum uwb_wake_decode_failure failure =
                app_anchor_wake_failure_from_proto_ret(ret);

            uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                failure);
            LOG_DBG("anchor ignored competing UWB WAKE_CLAIM decode failure: ret=%d reason=%s frame_len=%u",
                    ret,
                    app_anchor_wake_decode_failure_name(failure),
                    (unsigned int)frame_len);
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            !app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags,
                                                             true)) {
            status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DROP_DURING_CLICK src=0x%llx evt=%u attempt=%u flags=0x%02x\n",
                                (unsigned long long)claim.clicker_id,
                                claim.click_event_id,
                                claim.attempt_index,
                                claim.flags);
            continue;
        }

        ret = uwb_anchor_accept_wake_claim(&anchor_uwb_session,
                                           &claim,
                                           k_uptime_get_32(),
                                           &decision);
        HIGH_DEBUG_COUNTER_INC(wake_claim_rx);
        LOG_DBG("anchor UWB claim arbitration: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx ret=%d decision=%u",
                (unsigned long long)claim.clicker_id,
                claim.click_event_id,
                claim.attempt_index,
                (unsigned long long)claim.priority_id,
                ret,
                decision);
        if (ret == PROTO_OK &&
            (decision == UWB_ANCHOR_CLAIM_ACCEPTED ||
             decision == UWB_ANCHOR_CLAIM_REPLACED_BY_PRIORITY)) {
            HIGH_DEBUG_COUNTER_INC(wake_claim_accepted);
            selected_claim = claim;
            selected_rx_ms = k_uptime_get();
            selected_quality = quality;
            selected_decision = decision;
            stage1_led_result(STAGE1_LED_RESULT_OK);
            high_debug_log_event("WAKE_CLAIM_ACCEPT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx reason=%s",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 (unsigned long long)claim.priority_id,
                                 claim_decision_name(decision));
        } else {
            HIGH_DEBUG_COUNTER_INC(wake_claim_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("WAKE_CLAIM_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=%s ret=%d",
                                 (unsigned long long)claim.clicker_id,
                                 claim.click_event_id,
                                 claim.attempt_index,
                                 claim_decision_name(decision),
                                 ret);
        }
    }

    LOG_INF("anchor selected UWB claim: clicker=0x%016llx event_seq=%u attempt=%u priority=0x%016llx decision=%u",
            (unsigned long long)anchor_uwb_session.epoch.clicker_id,
            anchor_uwb_session.epoch.click_event_id,
            anchor_uwb_session.epoch.attempt_index,
            (unsigned long long)anchor_uwb_session.epoch.priority_id,
            selected_decision);
    click_priority = app_mesh_c5_wake_claim_preempts_mesh(selected_claim.flags);
    if (click_priority && mesh_preempt_for_click_event() < 0) {
        LOG_ERR("anchor click claim deferred because mesh custody preemption failed");
        return false;
    }
    anchor_click_window_set_active(click_priority);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_WINDOW active=%u flags=0x%02x\n",
                            anchor_click_window_active() ? 1u : 0u,
                            selected_claim.flags);
    }

    discovery_uses_extended_phr =
        app_mesh_c5_wake_followup_uses_extended_phr(selected_claim.flags);
    discovery_late_guard_ms = discovery_uses_extended_phr ?
                              MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS :
                              UWB_DISCOVERY_RX_LATE_GUARD_MS;
    discovery_start_ms =
        (int64_t)selected_rx_ms + selected_claim.discovery_starts_in_ms;
    discovery_deadline_ms = discovery_start_ms + discovery_late_guard_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_WAIT now=%u start=%lld deadline=%lld guard=%u listen=%u phr=%s\n",
                            k_uptime_get_32(),
                            (long long)discovery_start_ms,
                            (long long)discovery_deadline_ms,
                            UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_late_guard_ms + UWB_DISCOVERY_RX_GUARD_MS,
                            discovery_uses_extended_phr ? "ext" : "std");
    }
    high_debug_log_event("DISCOVER_WAIT",
                         "clicker=0x%016llx event_seq=%u attempt=%u selected_rx_ms=%lld discovery_starts_in_ms=%u wait_start_ms=%lld deadline_ms=%lld listen_ms=%u",
                         (unsigned long long)selected_claim.clicker_id,
                         selected_claim.click_event_id,
                         selected_claim.attempt_index,
                         (long long)selected_rx_ms,
                         selected_claim.discovery_starts_in_ms,
                         (long long)(discovery_start_ms - UWB_DISCOVERY_RX_GUARD_MS),
                         (long long)discovery_deadline_ms,
                         UWB_DISCOVERY_LISTEN_MS);

    stage1_led_phase(STAGE1_LED_PHASE_DISCOVERY);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    ret = discovery_uses_extended_phr ?
          dwm3000_driver_configure_wake_mesh_control_mode() :
          dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_DISC_CONFIG phr=%s ret=%d\n",
                            discovery_uses_extended_phr ? "ext" : "std",
                            ret);
    }
    if (ret < 0) {
        last_discovery_ret = ret;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        high_debug_log_event("DISCOVER_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u result=config-fail phr=%s ret=%d",
                             (unsigned long long)selected_claim.clicker_id,
                             selected_claim.click_event_id,
                             selected_claim.attempt_index,
                             discovery_uses_extended_phr ? "ext" : "std",
                             ret);
        goto discovery_miss;
    }

    while (k_uptime_get() < discovery_deadline_ms) {
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        uint8_t quality = 0u;
        int64_t remaining_ms = discovery_deadline_ms - k_uptime_get();

        ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                      frame,
                                                      sizeof(frame),
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
        if (ret == -ETIMEDOUT) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_TIMEOUT now=%u rem=%lld fail=%u\n",
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            break;
        }
        if (ret < 0) {
            last_discovery_ret = ret;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ANCHOR_DISC_RX_FAIL ret=%d now=%u rem=%lld fail=%u\n",
                                    ret,
                                    k_uptime_get_32(),
                                    (long long)(discovery_deadline_ms - k_uptime_get()),
                                    (unsigned int)rx_failure);
            }
            continue;
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_FRAME len=%u type=0x%02x q=%u now=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                k_uptime_get_32());
        }

        if ((selected_claim.flags & FLAG_RANGE_ONLY) != 0u &&
            frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_SCHEDULE) {
            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }

        last_discovery_decode_ret = uwb_decode_discover(frame, frame_len, &discover);
        if (last_discovery_decode_ret == PROTO_OK) {
            selected_quality = quality;
            discover_received = true;
            break;
        }

        if (frame_len == UWB_WAKE_CLAIM_LEN) {
            struct uwb_wake_claim_frame late_claim;

            ret = uwb_decode_wake_claim(frame, frame_len, &late_claim);
            if (ret == PROTO_OK) {
                LOG_DBG("anchor ignored late UWB WAKE_CLAIM while waiting for DISCOVER: clicker=0x%016llx event_seq=%u attempt=%u",
                        (unsigned long long)late_claim.clicker_id,
                        late_claim.click_event_id,
                        late_claim.attempt_index);
                continue;
            }
        }

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued && deferred_mesh_rx_queued != NULL) {
                    *deferred_mesh_rx_queued = true;
                }
                high_debug_log_event("MESH_CH5_AFTER_WAKE_RX",
                                     "clicker=0x%016llx event_seq=%u frame_len=%u quality=%u valid=%u",
                                     (unsigned long long)selected_claim.clicker_id,
                                     selected_claim.click_event_id,
                                     (unsigned int)frame_len,
                                     quality,
                                     valid_mesh_frame ? 1u : 0u);
                LOG_INF("anchor accepted channel-5 mesh frame after wake claim: clicker=0x%016llx event_seq=%u len=%u quality=%u valid=%u",
                        (unsigned long long)selected_claim.clicker_id,
                        selected_claim.click_event_id,
                        (unsigned int)frame_len,
                        quality,
                        valid_mesh_frame ? 1u : 0u);
                if (click_priority) {
                    continue;
                }
                return true;
            }
            status_debug_printf("DBG_ANCHOR_MESH_AFTER_WAKE_REJECT len=%u type=0x%02x q=%u discover_ret=%d valid=%u queued=%u\n",
                                (unsigned int)frame_len,
                                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                                quality,
                                last_discovery_decode_ret,
                                valid_mesh_frame ? 1u : 0u,
                                queued ? 1u : 0u);
        }

        LOG_DBG("anchor ignored non-DISCOVER frame during discovery wait: decode_ret=%d frame_len=%u type=0x%02x quality=%u",
                last_discovery_decode_ret,
                (unsigned int)frame_len,
                frame_len >= UWB_SYNC_HEADER_LEN ? frame[2] : 0u,
                quality);
    }

discovery_miss:
    if (discover_received) {
        struct uwb_discovery_reply_frame reply;
#if defined(CONFIG_IMEC_ML_ANCHOR)
        uint16_t discovery_battery_mv = ml_anchor_cached_battery_mv();
#else
        uint16_t discovery_battery_mv = 0u;
#endif

        HIGH_DEBUG_COUNTER_INC(discovery_rx);
        high_debug_log_event("DISCOVER_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u network_id=0x%08x channel=%u quality=%u",
                             (unsigned long long)discover.clicker_id,
                             discover.click_event_id,
                             discover.attempt_index,
                             discover.network_id,
                             UWB_WAKE_CHANNEL,
                             selected_quality);

        ret = uwb_anchor_build_discovery_reply(&anchor_uwb_session,
                                               &discover,
                                               selected_quality,
                                               discovery_battery_mv,
                                               &reply);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY build rejected: %d", ret);
            return true;
        }
#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (discover.discovery_slot_count > 0u) {
            reply.anchor_slot = ml_anchor_discovery_slot(discover.discovery_slot_count);
        }
#else
        enum app_discovery_assignment_provisioning_state provisioning_state =
            local_anchor_discovery_assignment_provisioning_state();

        ret = local_anchor_discovery_slot(discover.discovery_slot_count,
                                          &reply.anchor_slot);
        if (ret != PROTO_OK) {
            enum app_discovery_assignment_provisioning_state provisioning_state =
                local_anchor_discovery_assignment_provisioning_state();

            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            if (provisioning_state == APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED event=%u attempt=%u reply=disabled\n",
                                    discover.click_event_id,
                                    discover.attempt_index);
                LOG_WRN("anchor UWB DISCOVERY_REPLY suppressed: assignment UNPROVISIONED event=%u attempt=%u",
                        discover.click_event_id,
                        discover.attempt_index);
            } else {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_INVALID event=%u attempt=%u slot_count=%u ret=%d\n",
                                    discover.click_event_id,
                                    discover.attempt_index,
                                    discover.discovery_slot_count,
                                    ret);
                LOG_WRN("anchor UWB DISCOVERY_REPLY slot rejected: slot_count=%u ret=%d",
                        discover.discovery_slot_count,
                        ret);
            }
            return true;
        }
        if (provisioning_state == APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
            status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED event=%u attempt=%u reply=fallback slot=%u\n",
                                discover.click_event_id,
                                discover.attempt_index,
                                reply.anchor_slot);
            LOG_WRN("anchor UWB DISCOVERY_REPLY using deterministic fallback while UNPROVISIONED: event=%u attempt=%u slot=%u",
                    discover.click_event_id,
                    discover.attempt_index,
                    reply.anchor_slot);
        }
#endif

        sleep_precise_us((uint32_t)reply.anchor_slot * UWB_DISCOVERY_SLOT_US);
        ret = uwb_encode_discovery_reply(&reply, frame, sizeof(frame), &frame_len);
        if (ret != PROTO_OK) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY encode failed: %d", ret);
            return true;
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        UWB_DISCOVERY_REPLY_TX_TIMEOUT_MS);
        if (ret < 0) {
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB DISCOVERY_REPLY TX failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(discovery_reply_tx);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("DISCOVERY_REPLY_TX",
                             "clicker=0x%016llx event_seq=%u slot=%u slot_source=%s quality=%u",
                             (unsigned long long)reply.selected_clicker_id,
                             reply.click_event_id,
                             reply.anchor_slot,
                             ANCHOR_DISCOVERY_SLOT_SOURCE,
                             reply.rx_quality);
        LOG_INF("anchor UWB DISCOVERY_REPLY sent: clicker=0x%016llx slot=%u source=%s quality=%u",
                (unsigned long long)reply.selected_clicker_id,
                reply.anchor_slot,
                ANCHOR_DISCOVERY_SLOT_SOURCE,
                reply.rx_quality);
    } else if (schedule_frame_ready) {
        stage1_led_result(STAGE1_LED_RESULT_OK);
        LOG_INF("anchor fast range-only path: using direct RANGE_SCHEDULE without DISCOVER reply");
        high_debug_log_event("DISCOVER_SKIP",
                             "clicker=0x%016llx event_seq=%u attempt=%u reason=range_only_schedule",
                             (unsigned long long)selected_claim.clicker_id,
                             selected_claim.click_event_id,
                             selected_claim.attempt_index);
    } else {
        stage1_led_result(last_discovery_ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_DISC_MISS ret=%d decode=%d now=%u\n",
                                last_discovery_ret,
                                last_discovery_decode_ret,
                                k_uptime_get_32());
        }
        LOG_WRN("anchor UWB DISCOVER not received: ret=%d decode_ret=%d listen_ms=%u",
                last_discovery_ret,
                last_discovery_decode_ret,
                UWB_DISCOVERY_LISTEN_MS);
        return true;
    }

    stage1_led_phase(STAGE1_LED_PHASE_SCHEDULE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!schedule_frame_ready) {
        uint32_t schedule_rx_window_ms =
            UWB_RANGE_SCHEDULE_RX_MS +
            discovery_window_ms_for_slots(discover.discovery_slot_count) +
            UWB_DISCOVERY_RX_GUARD_MS;
        int64_t schedule_deadline_ms = k_uptime_get() + schedule_rx_window_ms;
        int last_schedule_ret = -ETIMEDOUT;

        while (k_uptime_get() < schedule_deadline_ms) {
            uint8_t frame_type = 0u;
            int64_t remaining_ms = schedule_deadline_ms - k_uptime_get();

            ret = dwm3000_driver_receive_frame_continuous((uint32_t)MAX(1, remaining_ms),
                                                          frame,
                                                          sizeof(frame),
                                                          &frame_len,
                                                          NULL,
                                                          NULL,
                                                          NULL);
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                last_schedule_ret = ret;
                continue;
            }
            if (frame_len < UWB_SYNC_HEADER_LEN) {
                last_schedule_ret = -EBADMSG;
                continue;
            }

            frame_type = frame[2];
            if (frame_type != MSG_UWB_RANGE_SCHEDULE &&
#if defined(CONFIG_IMEC_ML_ANCHOR)
                frame_type != MSG_UWB_ANCHOR_PAIR_SCHEDULE &&
#endif
                frame_type != MSG_UWB_RANGE_RELEASE) {
                last_schedule_ret = -EBADMSG;
                LOG_DBG("anchor ignored non-schedule frame while waiting for RANGE_SCHEDULE: type=0x%02x len=%u",
                        frame_type,
                        (unsigned int)frame_len);
                continue;
            }

            schedule_frame_ready = true;
            schedule_rx_ms = k_uptime_get();
            break;
        }
        if (!schedule_frame_ready) {
            ret = last_schedule_ret;
        }
    }
    if (ret == 0) {
        struct uwb_range_schedule_frame schedule;

        if (schedule_rx_ms < 0) {
            schedule_rx_ms = k_uptime_get();
        }

        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_RANGE_RELEASE) {
            struct uwb_range_release_frame release;

            ret = uwb_decode_range_release(frame, frame_len, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor UWB RANGE_RELEASE decode failed: %d", ret);
                return true;
            }

            ret = uwb_anchor_accept_range_release(&anchor_uwb_session, &release);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected UWB RANGE_RELEASE: %d", ret);
                return true;
            }

            stage1_led_result(STAGE1_LED_RESULT_OK);
            LOG_INF("anchor UWB RANGE_RELEASE accepted: clicker=0x%016llx event_seq=%u discovered=%u min=%u reason=%u",
                    (unsigned long long)release.clicker_id,
                    release.click_event_id,
                    release.discovered_anchor_count,
                    release.min_anchor_count,
                    release.reason);
            return true;
        }

#if defined(CONFIG_IMEC_ML_ANCHOR)
        if (frame_len >= UWB_SYNC_HEADER_LEN &&
            frame[2] == MSG_UWB_ANCHOR_PAIR_SCHEDULE) {
            struct uwb_anchor_pair_schedule_frame pair_schedule;

            ret = uwb_decode_anchor_pair_schedule(frame, frame_len, &pair_schedule);
            if (ret != PROTO_OK) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor pair-survey schedule decode failed: %d", ret);
                return true;
            }
            if (!anchor_pair_schedule_matches_epoch(&pair_schedule)) {
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor rejected pair-survey schedule identity: survey=%u",
                        pair_schedule.survey_id);
                return true;
            }
            stage1_led_result(STAGE1_LED_RESULT_OK);
            if (retained_sleep_us != NULL) {
                *retained_sleep_us = u32_saturating_add(
                    *retained_sleep_us,
                    anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms));
            } else {
                (void)anchor_run_clicker_pair_survey(&pair_schedule, schedule_rx_ms);
            }
            return true;
        }
#endif

        ret = uwb_decode_range_schedule(frame, frame_len, &schedule);
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_WRN("anchor UWB RANGE_SCHEDULE decode failed: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_rx);
        high_debug_log_event("RANGE_SCHEDULE_RX",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u reply_delay_uus=%u nonce=0x%016llx",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             schedule.reply_delay_us,
                             (unsigned long long)schedule.nonce);

        ret = uwb_anchor_accept_range_schedule(&anchor_uwb_session,
                                               &schedule,
                                               (uint32_t)schedule_rx_ms,
                                               UWB_SCHEDULE_GUARD_MS);
        if (ret == PROTO_ERR_NOT_FOUND) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            LOG_INF("anchor not selected in UWB RANGE_SCHEDULE: clicker=0x%016llx event_seq=%u",
                    (unsigned long long)schedule.clicker_id,
                    schedule.click_event_id);
            return true;
        }
        if (ret != PROTO_OK) {
            HIGH_DEBUG_COUNTER_INC(schedules_rejected);
            stage1_led_result(STAGE1_LED_RESULT_ERROR);
            high_debug_log_event("RANGE_SCHEDULE_REJECT",
                                 "clicker=0x%016llx event_seq=%u attempt=%u reason=proto_%d",
                                 (unsigned long long)schedule.clicker_id,
                                 schedule.click_event_id,
                                 schedule.attempt_index,
                                 ret);
            LOG_WRN("anchor rejected UWB RANGE_SCHEDULE: %d", ret);
            return true;
        }
        HIGH_DEBUG_COUNTER_INC(schedules_accepted);
        stage1_led_result(STAGE1_LED_RESULT_OK);
        high_debug_log_event("RANGE_SCHEDULE_ACCEPT",
                             "clicker=0x%016llx event_seq=%u attempt=%u selected=%u burst_id=%u",
                             (unsigned long long)schedule.clicker_id,
                             schedule.click_event_id,
                             schedule.attempt_index,
                             schedule.selected_count,
                             uwb_schedule_burst_id(schedule.click_event_id,
                                                   schedule.attempt_index));

        if (retained_sleep_us != NULL) {
            *retained_sleep_us = u32_saturating_add(
                *retained_sleep_us,
                anchor_run_scheduled_uwb_ranges(&schedule,
                                                schedule_rx_ms,
                                                deferred_mesh_rx_queued));
        } else {
            (void)anchor_run_scheduled_uwb_ranges(&schedule,
                                                  schedule_rx_ms,
                                                  deferred_mesh_rx_queued);
        }
    } else {
        stage1_led_result(ret == -ETIMEDOUT ?
                          STAGE1_LED_RESULT_TIMEOUT :
                          STAGE1_LED_RESULT_ERROR);
        LOG_WRN("anchor UWB RANGE_SCHEDULE not received: ret=%d", ret);
    }
    return true;
}

static bool anchor_handle_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms)
{
    uint32_t retained_sleep_us = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    bool deferred_mesh_rx_queued = false;
    bool handled = false;
    int64_t uwb_window_start_ms = -1;
    int low_power_ret;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR || claim == NULL ||
        !app_mesh_c5_wake_claim_preempts_mesh(claim->flags)) {
        LOG_ERR("invalid anchor click handoff request");
        return false;
    }

    (void)k_work_cancel_delayable(&anchor_uwb_scan_work);
    anchor_click_window_set_active(true);
    if (mesh_preempt_for_click_event() < 0) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click handoff deferred because mesh custody preemption failed");
        return false;
    }
    ret = radio_guard_uwb_start("anchor mesh click handoff");
    if (ret < 0) {
        LOG_ERR("anchor click handoff could not acquire UWB radio: ret=%d", ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_GUARD_FAIL ret=%d\n", ret);
        }
        goto complete;
    }

    anchor_set_uwb_busy(true);
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        LOG_ERR("anchor click handoff wake PHY configuration failed: ret=%d", ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_CONFIG_FAIL ret=%d\n", ret);
        }
        goto release_radio;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_START evt=%u attempt=%u rx=%lld q=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            (long long)received_at_ms,
                            link_quality);
    }
    handled = anchor_handle_uwb_claim(claim,
                                      link_quality,
                                      received_at_ms,
                                      &retained_sleep_us,
                                      &deferred_mesh_rx_queued);
    if (handled) {
        uwb_anchor_abort_epoch(&anchor_uwb_session);
    }

release_radio:
    low_power_ret = anchor_enter_low_power(
        app_radio_low_power_mode_for_connection(
            mesh_anchor_connected_radio_active()),
        "click-release");
    if (low_power_ret < 0) {
        anchor_scan_recovery_gap_requested = true;
    }
    anchor_note_uwb_awake_since(uwb_window_start_ms, retained_sleep_us);
    radio_guard_uwb_stop();
    anchor_set_uwb_busy(false);

complete:
    anchor_click_window_set_active(false);
    if (deferred_mesh_rx_queued) {
        mesh_stop_role_scan();
        mesh_submit_queued_rx();
    }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    report_tx_schedule(0u);
#endif
    if (handled && anchor_scan_recovery_gap_requested &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS;
    }
    anchor_scan_recovery_gap_requested = false;
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF_DONE evt=%u attempt=%u handled=%u deferred=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            handled ? 1u : 0u,
                            deferred_mesh_rx_queued ? 1u : 0u);
    }
    return handled;
}

static void anchor_uwb_scan_work_handler(struct k_work *work)
{
    uint8_t *frame = anchor_uwb_scan_frame;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint32_t next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    uint32_t retained_sleep_us = 0u;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int64_t uwb_window_start_ms = -1;
    bool preamble_detected = false;
    bool focused_logs = stage1_anchor_focused_rx_logs_enabled();
    bool log_scan_window = false;
    uint32_t scan_debug_now_ms = 0u;
    int64_t focused_spin_deadline_ms = -1;
    bool handled_claim = false;
    bool deferred_mesh_rx_queued = false;
    bool route_wake_handoff = false;
    size_t route_wake_frame_len = 0u;
    uint8_t route_wake_quality = 0u;
    bool route_waiting_active = false;
    bool relay_tx_active = false;
    bool mesh_rx_active = false;
    bool ch9_rx_conflict = false;
    bool uwb_radio_busy = false;
    uint32_t ch9_retry_ms = 0u;
    uint32_t mesh_rx_queue_depth = 0u;
    bool mesh_rx_response_busy = false;
    uint32_t blocked_retry_ms = anchor_uwb_scan_blocked_retry_ms();
    uint32_t scan_rx_start_cycles = 0u;
    uint32_t scan_rx_elapsed_us = ANCHOR_UWB_SCAN_RX_US;
    uint32_t scan_extra_awake_us = 0u;
    int low_power_ret;
    int ret;

    ARG_UNUSED(work);
    app_watchdog_note_radio_progress();

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_HANDLER role=%u\n", DEVICE_ROLE);
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_rx_queue_depth = mesh_rx_pending_count();
        mesh_rx_response_busy = mesh_rx_response_active();
        mesh_rx_active = mesh_rx_response_busy;
    }
    ch9_rx_conflict = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                      mesh_anchor_low_duty_scan_should_defer(&ch9_retry_ms);
    uwb_radio_busy = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                     radio_guard_uwb_busy();
    if (anchor_uwb_window_active() ||
        app_anchor_survey_runtime_discovery_is_pending() ||
        relay_tx_active ||
        route_waiting_active ||
        mesh_rx_active ||
        ch9_rx_conflict ||
        uwb_radio_busy) {
        uint32_t retry_ms = mesh_rx_active ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            uwb_radio_busy ? ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS :
                            ch9_rx_conflict ? ch9_retry_ms : blocked_retry_ms;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_BLOCKED win=%u survey=%u tx=%u wait=%u rx=%u rxq=%u rxr=%u ch9=%u rf=%u retry=%u interval=%u\n",
                                anchor_uwb_window_active() ? 1u : 0u,
                                app_anchor_survey_runtime_discovery_is_pending() ? 1u : 0u,
                                relay_tx_active ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                mesh_rx_active ? 1u : 0u,
                                mesh_rx_queue_depth,
                                mesh_rx_response_busy ? 1u : 0u,
                                ch9_rx_conflict ? 1u : 0u,
                                uwb_radio_busy ? 1u : 0u,
                                retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(retry_ms);
        return;
    }

    ret = radio_guard_uwb_start("anchor low-duty UWB wake scan");
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret == -EBUSY) {
            blocked_retry_ms = ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_GUARD_FAIL ret=%d retry=%u interval=%u\n",
                                ret,
                                blocked_retry_ms,
                                anchor_uwb_scan_interval_ms);
        }
        anchor_uwb_scan_schedule_ms(blocked_retry_ms);
        return;
    }
    anchor_set_uwb_busy(true);
    anchor_click_window_set_active(false);
    uwb_window_start_ms = k_uptime_get();
    if (focused_logs) {
        focused_spin_deadline_ms = uwb_window_start_ms + ANCHOR_STAGE1_FOCUSED_RX_SPIN_MS;
    }

focused_scan_attempt:
    frame_len = 0u;
    quality = 0u;
    retained_sleep_us = 0u;
    rx_failure = DWM3000_RX_FAILURE_NONE;
    preamble_detected = false;
    next_scan_delay_ms = anchor_uwb_scan_interval_ms;
    stage1_led_phase(STAGE1_LED_PHASE_SCAN);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
    if (!focused_logs) {
        high_debug_log_event("UWB_RX_START",
                             "mode=anchor_wake_scan interval_ms=%u rx_window_ms=%u",
                             anchor_uwb_scan_interval_ms,
                             ANCHOR_UWB_SCAN_RX_MS);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        scan_debug_now_ms = k_uptime_get_32();
        log_scan_window = anchor_ch5_scan_debug_next_ms == 0u ||
                          uptime_deadline_reached(scan_debug_now_ms,
                                                  anchor_ch5_scan_debug_next_ms);
        if (log_scan_window) {
            anchor_ch5_scan_debug_next_ms =
                scan_debug_now_ms + ANCHOR_CH5_SCAN_DEBUG_INTERVAL_MS;
            status_debug_printf("DBG_ANCHOR_CH5_SCAN_ARM now=%u win=%u complete=%u interval=%u wait=%u tx=%u\n",
                                scan_debug_now_ms,
                                ANCHOR_UWB_SCAN_RX_MS,
                                ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
                                anchor_uwb_scan_interval_ms,
                                mesh_route_waiting_tx_active() ? 1u : 0u,
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u);
        }
    }

    ret = dwm3000_driver_configure_wake_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && ret != 0) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_CONFIG_FAIL ret=%d\n", ret);
    }
    if (ret == 0) {
        scan_rx_start_cycles = k_cycle_get_32();
        ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
            ANCHOR_UWB_SCAN_RX_MS,
            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
            frame,
            UWB_MESH_MAX_FRAME_LEN,
            &frame_len,
            &quality,
            NULL,
            &rx_failure);
        scan_rx_elapsed_us = (uint32_t)k_cyc_to_us_floor64(
            (uint32_t)(k_cycle_get_32() - scan_rx_start_cycles));
        if (scan_rx_elapsed_us < ANCHOR_UWB_SCAN_RX_US) {
            scan_rx_elapsed_us = ANCHOR_UWB_SCAN_RX_US;
        }
    }
    preamble_detected = ret == 0 ||
                        app_anchor_rx_failure_detected_preamble(rx_failure);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (log_scan_window || ret == 0 || ret != -ETIMEDOUT ||
         preamble_detected)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_DONE ret=%d len=%u fail=%u pre=%u q=%u rx_us=%u complete=%u\n",
                            ret,
                            (unsigned int)frame_len,
                            (unsigned int)rx_failure,
                            preamble_detected ? 1u : 0u,
                            quality,
                            scan_rx_elapsed_us,
                            ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS);
    }
    uwb_anchor_note_idle_scan(&anchor_uwb_session,
                              ANCHOR_UWB_STARTUP_US,
                              ANCHOR_UWB_PLL_US,
                              scan_rx_elapsed_us,
                              preamble_detected);

    if (ret == 0) {
        struct uwb_wake_claim_frame claim;
        int decode_ret;

        stage1_led_result(STAGE1_LED_RESULT_OK);
        status_debug_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
        high_debug_log_event("UWB_RX_DONE",
                             "mode=anchor_wake_scan frame_len=%u quality=%u rx_failure=%s",
                             (unsigned int)frame_len,
                             quality,
                             app_anchor_rx_failure_name(rx_failure));
        decode_ret = uwb_decode_wake_claim(frame, frame_len, &claim);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_FRAME len=%u q=%u wake_ret=%d\n",
                                (unsigned int)frame_len,
                                quality,
                                decode_ret);
        }
        stage1_anchor_focused_note_rx_frame(frame_len, quality, decode_ret);
        if (decode_ret == PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                !app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags,
                                                                 true)) {
                route_wake_handoff = true;
                route_wake_frame_len = frame_len;
                route_wake_quality = quality;
                status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DISPATCH src=0x%llx evt=%u attempt=%u flags=0x%02x len=%u q=%u\n",
                                    (unsigned long long)claim.clicker_id,
                                    claim.click_event_id,
                                    claim.attempt_index,
                                    claim.flags,
                                    (unsigned int)frame_len,
                                    quality);
                goto scan_complete;
            }
            stage1_anchor_focused_note_wake_claim(&claim);
            if (anchor_handle_uwb_claim(&claim,
                                        quality,
                                        k_uptime_get(),
                                        &retained_sleep_us,
                                        &deferred_mesh_rx_queued)) {
                handled_claim = true;
                uwb_anchor_abort_epoch(&anchor_uwb_session);
            }
        } else {
            bool valid_mesh_frame = false;
            bool queued;

            queued = mesh_queue_from_frame_deferred(frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (queued || valid_mesh_frame) {
                if (queued) {
                    deferred_mesh_rx_queued = true;
                }
                goto scan_complete;
            }
            {
                enum uwb_wake_decode_failure failure =
                    app_anchor_wake_failure_from_proto_ret(decode_ret);

                uwb_anchor_note_wake_decode_failure(&anchor_uwb_session,
                                                    failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
                next_scan_delay_ms = 0u;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor high-duty UWB wake ignored bad frame: wake_decode_ret=%d reason=%s frame_len=%u quality=%u retry_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        app_anchor_wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#else
                uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
                next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
                stage1_led_result(STAGE1_LED_RESULT_ERROR);
                LOG_WRN("anchor false UWB wake cooldown: wake_decode_ret=%d reason=%s frame_len=%u quality=%u cooldown_ms=%u crc_failures=%u frame_timeouts=%u",
                        decode_ret,
                        app_anchor_wake_decode_failure_name(failure),
                        (unsigned int)frame_len,
                        quality,
                        next_scan_delay_ms,
                        anchor_uwb_session.diagnostics.crc_failures,
                        anchor_uwb_session.diagnostics.frame_timeouts);
#endif
            }
        }
    } else if (ret != -ETIMEDOUT) {
        enum uwb_wake_decode_failure failure =
            app_anchor_wake_failure_from_rx(rx_failure);

        uwb_anchor_note_wake_decode_failure(&anchor_uwb_session, failure);
#if IS_ENABLED(CONFIG_IMEC_STAGE1_ANCHOR_CONTINUOUS_SCAN)
        next_scan_delay_ms = 0u;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        if (!focused_logs) {
            LOG_WRN("anchor high-duty UWB wake scan retry after activity: ret=%d rx_failure=%s reason=%s retry_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                    ret,
                    app_anchor_rx_failure_name(rx_failure),
                    app_anchor_wake_decode_failure_name(failure),
                    next_scan_delay_ms,
                    anchor_uwb_session.diagnostics.preambles,
                    anchor_uwb_session.diagnostics.sfd_timeouts,
                    anchor_uwb_session.diagnostics.frame_timeouts,
                    anchor_uwb_session.diagnostics.crc_failures);
        }
#else
        uwb_anchor_note_false_wake_cooldown(&anchor_uwb_session);
        next_scan_delay_ms = ANCHOR_FALSE_WAKE_COOLDOWN_MS;
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_CH5_ACTIVITY_COOLDOWN ret=%d fail=%u reason=%s rx_us=%u cooldown=%u\n",
                                ret,
                                (unsigned int)rx_failure,
                                app_anchor_wake_decode_failure_name(failure),
                                scan_rx_elapsed_us,
                                next_scan_delay_ms);
        }
        LOG_WRN("anchor UWB wake scan failure cooldown: ret=%d rx_failure=%s reason=%s cooldown_ms=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u",
                ret,
                app_anchor_rx_failure_name(rx_failure),
                app_anchor_wake_decode_failure_name(failure),
                next_scan_delay_ms,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures);
#endif
    }
    if (ret == -ETIMEDOUT) {
        stage1_led_result(STAGE1_LED_RESULT_TIMEOUT);
        if (!focused_logs) {
            high_debug_log_event("UWB_RX_TIMEOUT",
                                 "mode=anchor_wake_scan rx_failure=%s preamble=%u",
                                 app_anchor_rx_failure_name(rx_failure),
                                 preamble_detected ? 1u : 0u);
        }
    }

    if (focused_logs &&
        ret != 0 &&
        focused_spin_deadline_ms > 0 &&
        k_uptime_get() < focused_spin_deadline_ms &&
        !anchor_uwb_window_active() &&
        !app_anchor_survey_runtime_discovery_is_pending() &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        goto focused_scan_attempt;
    }

scan_complete:
    if (scan_rx_elapsed_us > ANCHOR_UWB_SCAN_RX_US) {
        scan_extra_awake_us = scan_rx_elapsed_us - ANCHOR_UWB_SCAN_RX_US;
    }
    low_power_ret = anchor_enter_low_power(
        app_radio_low_power_mode_for_connection(
            mesh_anchor_connected_radio_active()),
        "low-duty-scan-release");
    if (low_power_ret < 0 && next_scan_delay_ms < REPORT_TX_RETRY_DELAY_MS) {
        next_scan_delay_ms = REPORT_TX_RETRY_DELAY_MS;
    }
    anchor_note_uwb_awake_since(
        uwb_window_start_ms,
        u32_saturating_add(
            u32_saturating_add(ANCHOR_UWB_IDLE_SCAN_AWAKE_US, scan_extra_awake_us),
            retained_sleep_us));
    radio_guard_uwb_stop();
    anchor_set_uwb_busy(false);
    anchor_click_window_set_active(false);
    if (route_wake_handoff) {
        bool handed_off = mesh_anchor_handoff_route_wake_frame(
            frame,
            route_wake_frame_len,
            route_wake_quality);

        status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_DISPATCH_DONE handled=%u len=%u q=%u\n",
                            handed_off ? 1u : 0u,
                            (unsigned int)route_wake_frame_len,
                            route_wake_quality);
        if (!handed_off) {
            LOG_ERR("route-class wake claim handoff failed after anchor scan released radio");
        }
        if (next_scan_delay_ms < ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS) {
            next_scan_delay_ms = ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS;
        }
    }
    if (deferred_mesh_rx_queued &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_DEFERRED_MESH_RX_GAP_MS;
    }
    if (deferred_mesh_rx_queued) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ANCHOR_MESH_RX_HANDOFF_DEFERRED q=%u delay=%u rf=%u\n",
                                mesh_rx_pending_count(),
                                next_scan_delay_ms,
                                radio_guard_uwb_busy() ? 1u : 0u);
        }
        mesh_stop_role_scan();
        mesh_submit_queued_rx();
    }
#if !defined(CONFIG_IMEC_ML_ANCHOR)
    report_tx_schedule(0u);
#endif
    if (handled_claim &&
        anchor_scan_recovery_gap_requested &&
        next_scan_delay_ms < ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS) {
        next_scan_delay_ms = ANCHOR_UWB_SCAN_POST_SEQUENCE_IDLE_MS;
    }
    anchor_scan_recovery_gap_requested = false;
    if (focused_logs) {
        stage1_anchor_focused_log_diagnostics(ret,
                                              app_anchor_rx_failure_name(rx_failure),
                                              preamble_detected,
                                              &anchor_uwb_session);
    } else {
        LOG_DBG("anchor UWB diagnostics: last_ret=%d last_rx_failure=%s last_preamble=%u scans=%u preambles=%u sfd_timeouts=%u frame_timeouts=%u crc_failures=%u claims=%u collisions=%u wins=%u losses=%u replies=%u schedules=%u ds_ok=%u ds_fail=%u timing_rejections=%u mesh_packets=%u sample_order=%u scan_startup_us=%u scan_pll_us=%u scan_rx_us=%u awake_us=%u false_cooldowns=%u",
                ret,
                app_anchor_rx_failure_name(rx_failure),
                preamble_detected ? 1u : 0u,
                anchor_uwb_session.diagnostics.scans,
                anchor_uwb_session.diagnostics.preambles,
                anchor_uwb_session.diagnostics.sfd_timeouts,
                anchor_uwb_session.diagnostics.frame_timeouts,
                anchor_uwb_session.diagnostics.crc_failures,
                anchor_uwb_session.diagnostics.claims,
                anchor_uwb_session.diagnostics.collisions,
                anchor_uwb_session.diagnostics.arbitration_wins,
                anchor_uwb_session.diagnostics.arbitration_losses,
                anchor_uwb_session.diagnostics.discovery_replies,
                anchor_uwb_session.diagnostics.schedules,
                anchor_uwb_session.diagnostics.ds_twr_successes,
                anchor_uwb_session.diagnostics.ds_twr_failures,
                anchor_uwb_session.diagnostics.timing_rejections,
                anchor_uwb_session.diagnostics.uwb_mesh_packets,
                anchor_uwb_session.diagnostics.sample_order_count,
                anchor_uwb_session.diagnostics.scan_startup_time_us,
                anchor_uwb_session.diagnostics.scan_pll_time_us,
                anchor_uwb_session.diagnostics.scan_rx_time_us,
                anchor_uwb_session.diagnostics.awake_time_us,
                anchor_uwb_session.diagnostics.false_wake_cooldowns);
        high_debug_log_event("UWB_SLEEP",
                             "mode=anchor_wake_scan next_delay_ms=%u retained_sleep_us=%u",
                             next_scan_delay_ms,
                             retained_sleep_us);
    }
    anchor_uwb_scan_schedule_ms(next_scan_delay_ms);
}

static int anchor_start_uwb_scan(void)
{
    uint32_t startup_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ? 2000u : 0u;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        LOG_INF("mesh-test source transmitter skipping anchor low-duty UWB scan");
        status_debug_note("DBG_ANCHOR_SCAN_SKIPPED_TX\n");
        return 0;
    }

    anchor_uwb_scan_schedule_ms(startup_delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CH5_SCAN_SCHEDULED startup=%u interval=%u win=%u\n",
                            startup_delay_ms,
                            anchor_uwb_scan_interval_ms,
                            ANCHOR_UWB_SCAN_RX_MS);
    }
    LOG_INF("anchor low-duty UWB wake scan scheduled: startup_delay_ms=%u interval_ms=%u rx_window_ms=%u hash_slots=%u",
            startup_delay_ms,
            anchor_uwb_scan_interval_ms,
            ANCHOR_UWB_SCAN_RX_MS,
            UWB_DISCOVERY_SLOT_COUNT);
    return 0;
}


static const struct app_mesh_report_callbacks anchor_mesh_report_callbacks = {
    .anchor_survey_discovery_is_pending =
        app_anchor_survey_runtime_discovery_is_pending,
    .anchor_note_uwb_awake_since = anchor_note_uwb_awake_since,
    .anchor_handle_click_wake_claim = anchor_handle_mesh_click_wake_claim,
    .anchor_handle_local_command = anchor_handle_local_command,
    .anchor_handle_survey_discovery_start =
        app_anchor_survey_discovery_handle_start,
    .anchor_handle_survey_pair_prepare =
        app_anchor_survey_runtime_handle_pair_prepare,
    .gateway_handle_survey_discovery_report = gateway_handle_survey_discovery_report,
    .anchor_survey_delivery_gateway_confirmed =
        app_anchor_survey_delivery_gateway_confirmed,
    .anchor_survey_delivery_transport_released =
        app_anchor_survey_delivery_transport_released,
    .gateway_route_refresh_event = gateway_route_refresh_observe,
};

const struct app_mesh_report_callbacks *app_anchor_mesh_report_callbacks(void)
{
    return &anchor_mesh_report_callbacks;
}

int app_anchor_init(void)
{
    const struct app_anchor_command_completion_ops completion_ops = {
        .force_rediscovery = anchor_force_rediscovery_from_command,
        .schedule_reboot = anchor_schedule_reboot_after_command_result,
    };
    const struct app_anchor_survey_runtime_ops runtime_ops = {
#if DEVICE_ROLE == ROLE_ANCHOR
        .work_queue = &anchor_uwb_scan_work_q,
#endif
        .send_command_result = anchor_send_command_result,
        .enter_low_power = anchor_enter_low_power,
        .set_uwb_busy = anchor_set_uwb_busy,
        .note_uwb_awake_since = anchor_note_uwb_awake_since,
        .start_uwb_scan = anchor_start_uwb_scan,
        .queue_sample_result = anchor_queue_survey_sample_result,
        .report_queue_used = report_tx_queue_used,
        .report_schedule = report_tx_schedule,
        .relay_tx_active = anchor_relay_tx_active,
        .connected_radio_active = mesh_anchor_connected_radio_active,
    };
    const struct app_anchor_survey_discovery_ops discovery_ops = {
        .abort_requested = app_anchor_survey_runtime_abort_requested,
        .abort_pair = app_anchor_survey_runtime_abort_pair,
        .preempt_radio = anchor_preempt_for_survey_discovery,
        .queue_start = app_anchor_survey_runtime_queue_discovery,
        .schedule_work_ms = app_anchor_survey_runtime_schedule_ms,
        .next_sequence = app_anchor_survey_runtime_next_sequence,
    };
    int ret;

    ret = app_anchor_command_completion_init(&completion_ops);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_survey_runtime_init(&runtime_ops);
    if (ret < 0) {
        return ret;
    }
    ret = app_anchor_survey_discovery_init(&discovery_ops);
    if (ret < 0) {
        return ret;
    }
    k_work_init_delayable(&gateway_survey_work, gateway_survey_work_handler);
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
    if (gateway_host_command_msgq.max_msgs != GATEWAY_HOST_COMMAND_QUEUE_DEPTH) {
        return -EINVAL;
    }
    ret = app_gateway_command_lifecycle_init(&gateway_host_command_lifecycle,
                                             GATEWAY_HOST_COMMAND_QUEUE_DEPTH);
    if (ret < 0) {
        return ret;
    }
    k_work_init_delayable(&gateway_host_command_work,
                          gateway_host_command_work_handler);
    k_work_init_delayable(&gateway_host_command_retry_work,
                          gateway_host_command_retry_work_handler);
    app_mesh_arbitration_zephyr_gateway_set_schedule_failure_handler(
        gateway_host_command_schedule_failed, NULL);
#endif
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
#if DEVICE_ROLE == ROLE_GATEWAY
    {
        const struct app_gateway_assignment_publisher_ops publisher_ops = {
            .emit_if_available = gateway_observe_command_event_if_available,
        };

        ret = app_gateway_assignment_publisher_init(&publisher_ops);
        if (ret < 0) {
            return ret;
        }
    }
    k_work_init_delayable(&gateway_discovery_assignment_finalize_work,
                          gateway_discovery_assignment_finalize_work_handler);
    k_work_init_delayable(&gateway_discovery_assignment_publish_work,
                          gateway_discovery_assignment_publish_work_handler);
    app_discovery_assignment_work_guard_init(
        &gateway_discovery_assignment_publish_guard);
#endif
    return 0;
}

int app_anchor_start_anchor_role(void)
{
    const struct uwb_anchor_config anchor_config = {
        .network_id = NETWORK_ID,
        .anchor_id = DEVICE_ID,
        .wake_channel = UWB_WAKE_CHANNEL,
        .ranging_channel = UWB_RANGING_CHANNEL,
    };
    int ret;
    bool delivery_restored = false;

    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_ANCHOR,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    ret = app_mesh_persistence_restore_outbox(&mesh_runtime, k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh outbox restore unavailable: %d", ret);
    }
    mesh_report_resume_restored_outbox("anchor-startup");
    ret = app_anchor_survey_discovery_restore(&delivery_restored);
    if (ret < 0) {
        return ret;
    }
    ret = app_mesh_persistence_restore_child_custody(&mesh_runtime,
                                                    k_uptime_get_32());
    if (ret < 0) {
        LOG_WRN("anchor mesh child custody restore unavailable: %d", ret);
    }
    if (anchor_discovery_assignment_required()) {
        struct app_mesh_discovery_assignment_snapshot snapshot = {0};
        const char *unprovisioned_source = "erased-nvs";
        int restore_ret;

        local_anchor_reset_discovery_assignment();
        restore_ret = app_mesh_persistence_restore_discovery_assignment(&snapshot);
        if (restore_ret == 0 && snapshot.valid &&
            snapshot.local_id == DEVICE_ID && snapshot.gateway_id == GATEWAY_ID) {
            ret = local_anchor_restore_discovery_assignment(snapshot.epoch,
                                                            snapshot.slot,
                                                            snapshot.slot_count);
            if (ret == PROTO_OK) {
                status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=PROVISIONED source=nvs epoch=%u slot=%u slots=%u\n",
                                    snapshot.epoch,
                                    snapshot.slot,
                                    snapshot.slot_count);
                LOG_INF("anchor discovery assignment restored: epoch=%u slot=%u slot_count=%u",
                        snapshot.epoch,
                        snapshot.slot,
                        snapshot.slot_count);
            } else {
                unprovisioned_source = "invalid-nvs";
                app_mesh_persistence_clear_discovery_assignment();
                local_anchor_reset_discovery_assignment();
            }
        } else if (restore_ret == 0) {
            unprovisioned_source = "invalid-nvs";
            app_mesh_persistence_clear_discovery_assignment();
        } else if (restore_ret == -ENOTSUP) {
            unprovisioned_source = "nvs-disabled";
        } else if (restore_ret != -ENOENT) {
            unprovisioned_source = "nvs-error";
            LOG_WRN("anchor discovery assignment restore unavailable: %d",
                    restore_ret);
        }
        if (local_anchor_discovery_assignment_provisioning_state() ==
            APP_DISCOVERY_ASSIGNMENT_UNPROVISIONED) {
            status_debug_printf("DBG_DISCOVERY_ASSIGNMENT_STATUS state=UNPROVISIONED source=%s restore=%d reply=disabled\n",
                                unprovisioned_source,
                                restore_ret);
            LOG_WRN("anchor discovery assignment UNPROVISIONED: source=%s restore=%d; normal click replies disabled until gateway assignment",
                    unprovisioned_source,
                    restore_ret);
        }
    }
    ret = uwb_anchor_session_init(&anchor_uwb_session, &anchor_config);
    if (ret < 0) {
        LOG_ERR("anchor UWB session init failed: %d", ret);
        return ret;
    }
    k_work_init_delayable(&anchor_uwb_scan_work, anchor_uwb_scan_work_handler);
    k_work_init_delayable(&anchor_heartbeat_work, anchor_heartbeat_work_handler);
    k_work_init_delayable(&anchor_reboot_work, anchor_reboot_work_handler);
    ret = app_anchor_survey_runtime_start();
    if (ret < 0) {
        return ret;
    }
    k_work_init_delayable(&anchor_collection_result_work,
                          anchor_collection_result_work_handler);
    k_work_init_delayable(&anchor_command_execute_work,
                          anchor_command_execute_work_handler);
    k_work_init_delayable(&anchor_discovery_claim_work,
                          anchor_discovery_claim_work_handler);
    if (mesh_relay_tx_active(&mesh_runtime)) {
        app_mesh_persistence_clear_collection_result();
    } else {
        ret = anchor_collection_result_restore();
        if (ret < 0 && ret != -ENOENT && ret != -ENOTSUP) {
            LOG_WRN("anchor collection result restore unavailable: %d", ret);
        }
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (gateway_ble_transport_enabled()) {
        status_debug_note("DBG_ANCHOR_BLE_INIT_BEGIN\n");
        ret = gateway_ble_init();
        status_debug_note("DBG_ANCHOR_BLE_INIT_DONE\n");
        if (ret < 0) {
            LOG_ERR("mesh-test anchor BLE debug log link unavailable: %d", ret);
        } else {
            LOG_INF("mesh-test anchor BLE debug log link ready before UWB scan");
        }
    } else {
        status_debug_note("DBG_ANCHOR_BLE_DISABLED\n");
    }
#endif
#if defined(CONFIG_IMEC_ML_ANCHOR)
    (void)app_ml_init();
#endif
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_queue_start(&anchor_uwb_scan_work_q,
                       anchor_uwb_scan_work_q_stack,
                       K_THREAD_STACK_SIZEOF(anchor_uwb_scan_work_q_stack),
                       ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
                       &anchor_uwb_scan_work_q_config);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_ANCHOR_CH5_SCAN_WQ_STARTED\n");
    }
#endif
    if (delivery_restored) {
        app_anchor_survey_runtime_schedule_ms(0u);
    }
    ret = anchor_start_uwb_scan();
    if (ret < 0) {
        LOG_ERR("anchor UWB scan unavailable: %d", ret);
    }
    return 0;
}

int app_anchor_start_gateway_role(void)
{
    mesh_relay_init(&mesh_runtime,
                    MESH_RELAY_ROLE_GATEWAY,
                    DEVICE_ID,
                    GATEWAY_ID,
                    1u);
    return 0;
}
