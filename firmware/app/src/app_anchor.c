#include "app_anchor.h"
#include "app_anchor_command_completion.h"
#include "app_anchor_ranging.h"

#include "app_radio_low_power_policy.h"
#include "app_board.h"
#include "app_config.h"
#include "app_discovery_assignment_policy.h"
#include "app_discovery_assignment_stack.h"
#include "app_durable_state.h"
#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_command_ingress.h"
#include "app_gateway_command_lifecycle.h"
#include "app_gateway_command_result.h"
#include "app_gateway_control_sequence.h"
#include "app_gateway_operation_owner.h"
#include "app_mesh_arbitration_zephyr.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_report.h"
#include "app_mesh_rx_policy.h"
#include "app_ml.h"
#include "app_ram_state_types.h"
#include "app_node_comm.h"
#include "app_node_comm_gateway_route_refresh.h"
#include "app_operation_policy.h"
#include "app_state.h"
#include "app_stack_workload_diag.h"
#include "app_survey.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "discovery_assignment.h"
#include "enumeration_response_lane.h"
#include "gateway_command.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "protocol_rx_lifecycle.h"
#include "report.h"
#include "route.h"
#include "serial_frame.h"
#include "status.h"
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
#define ANCHOR_COMMAND_DELIVERY_POLL_MS 5u
#define ANCHOR_DISCOVERY_ACK_ROUTE_WAIT_RETRY_MS 500u
#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS 60000u
#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS 3600000u
#define GATEWAY_HOST_COMMAND_QUEUE_DEPTH 2u
#define GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS 8u
#define GATEWAY_HOST_COMMAND_ACK_CONFIRM_POLL_MS 1000u
#define GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS
#define GATEWAY_DISCOVERY_ASSIGNMENT_ABORT_DELIVERY_BUDGET_MS 10000u
BUILD_ASSERT(UWB_DISCOVERY_SLOT_COUNT == MESH_CONNECTED_MAX_ANCHORS,
             "gateway enumeration must cover the connected anchor maximum");
BUILD_ASSERT(ENUMERATION_RESPONSE_START_DELAY_MS +
                 ENUMERATION_RESPONSE_LANE_MS <
                 DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
             "pipelined CLAIM and response lane must fit the operation budget");
BUILD_ASSERT(ENUMERATION_RESPONSE_START_DELAY_MS >=
                 DISCOVERY_ASSIGNMENT_CONTROL_PROPAGATION_MARGIN_MS +
                 (UWB_ENUM_MAX_HOPS *
                  MESH_ENUMERATION_CLAIM_RELAY_HOP_MAX_MS) +
                 ENUMERATION_RESPONSE_GATEWAY_PREPARE_MS,
             "response edge must follow every pipelined CLAIM hop");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS >=
             DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS,
             "default assignment budget must cover claim and table response horizons");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS >=
             DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *
                 GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS,
             "assignment budget must cover every control terminal poll");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS <=
             GATEWAY_COMMAND_BUDGET_MAX_MS,
             "default assignment budget must fit the shared command budget limit");
BUILD_ASSERT((DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S * 1000u) >=
             DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS,
             "assignment command expiry must cover the maximum response custody horizon");
BUILD_ASSERT(GATEWAY_COLLECTION_RESULT_CACHE_SIZE == MESH_CONNECTED_MAX_ANCHORS,
             "gateway collection storage must cover every connected anchor");
BUILD_ASSERT(APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES ==
             MESH_CONNECTED_MAX_ANCHORS,
             "gateway assignment publication must cover every connected anchor");
BUILD_ASSERT((uint64_t)UINT16_MAX *
                 (uint64_t)ANCHOR_HEARTBEAT_MIN_INTERVAL_MS >
             (uint64_t)MESH_RELAY_GATEWAY_ACK_RETENTION_MS +
                 (uint64_t)ANCHOR_HEARTBEAT_DELIVERY_TIMEOUT_MS,
             "heartbeat sequence must not wrap inside retained gateway identity custody");
BUILD_ASSERT(APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_BYTES <=
             APP_DISCOVERY_ASSIGNMENT_PUBLISH_LARGE_LOCAL_LIMIT_BYTES,
             "gateway discovery table publisher must stay below a 4 KiB stack frame");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS >
             GATEWAY_COMMAND_RESULT_TIMEOUT_MS,
             "assignment response custody must outlive the generic command-result deadline");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS >=
             NODE_COMM_PROTOCOL_RESPONSE_MAX_ATTEMPTS *
                 (NODE_COMM_PROTOCOL_RESPONSE_RETRY_BACKOFF_MAX_MS +
                  ROUTE_GATEWAY_ACK_TIMEOUT_MS),
             "assignment response custody must cover every bounded protocol-response retry");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_ACK_FAST_HANDLE_RETRIES > 0u &&
             ANCHOR_DISCOVERY_ACK_ROUTE_WAIT_RETRY_MS > 0u &&
             ANCHOR_DISCOVERY_ACK_ROUTE_WAIT_RETRY_MS <
                 ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS &&
             ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS >
                 DISCOVERY_ASSIGNMENT_RETRY_MAX_MS &&
             ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS >=
                 ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS,
             "assignment ACK low-duty retry must be bounded and slower than fast recovery");

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(UWB_RANGE_SCHEDULE_MAX_LEN <= UWB_MESH_MAX_FRAME_LEN,
             "post-wake route RX buffer must still fit normal ranging schedules");
BUILD_ASSERT(ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE >= 8192u,
             "mesh-route anchor scan must retain its measured safety margin");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_PRIORITY < ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
             "mesh route work must preempt low-duty anchor scan handoff");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_STACK_SIZE >= 9472u,
             "mesh communication worker must retain its measured route stack budget");
BUILD_ASSERT(ANCHOR_UWB_SCAN_BUSY_RETRY_MS > 0u,
             "blocked mesh route-test anchor scans must not spin at zero delay");
#endif

static struct k_work_delayable anchor_uwb_scan_work;
static struct k_work anchor_click_handoff_work;
static struct k_work_delayable anchor_heartbeat_work;
static struct k_work_delayable anchor_reboot_work;
static struct k_work_delayable anchor_collection_result_work;
static struct k_work_delayable anchor_command_execute_work;
static struct k_work_delayable anchor_discovery_claim_work;
#if DEVICE_ROLE == ROLE_GATEWAY
static struct k_work_delayable gateway_discovery_assignment_finalize_work;
static struct k_work_delayable gateway_discovery_assignment_publish_work;
#endif

#define GATEWAY_DISCOVERY_ASSIGNMENT_VALIDATION_RECHECK_MS 10u

struct anchor_discovery_claim_pending {
    struct proto_packet command;
    uint32_t epoch;
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t delivery_handle;
    uint32_t generation;
    uint64_t first_contact_origin_ms;
    uint64_t absolute_deadline_ms;
    uint64_t next_attempt_not_before_ms;
    uint32_t first_contact_random;
    uint16_t response_spread_ms;
    enum discovery_assignment_phase phase;
    /* Durable slot identifies the anchor; response_lane only schedules RF. */
    uint8_t slot;
    uint8_t slot_count;
    uint8_t response_lane;
    uint8_t response_lane_count;
    uint8_t hop_count;
    uint8_t attempt;
    uint8_t terminal_retry_count;
    bool ack_delivered;
    bool active;
};

struct anchor_enumeration_response_config {
    uint64_t start_ms;
    uint64_t parent_id;
    uint32_t epoch;
    uint8_t hop_count;
    uint8_t max_hop_count;
    bool active;
};

/*
 * RAM-only proof that this exact response completed the full gateway
 * ACK-confirm handshake.  It deliberately does not survive reset: after a
 * reboot an exact command replay must be allowed to reconstruct custody.
 */
struct anchor_discovery_response_terminal {
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t epoch;
    uint32_t command_session_id;
    uint16_t command_packet_seq;
    enum discovery_assignment_phase phase;
    bool valid;
};

#if DEVICE_ROLE == ROLE_GATEWAY
enum gateway_discovery_assignment_stage {
    GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS = 0,
    GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_PROPAGATION = 1,
    GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_ABORT_DELIVERY = 2,
};

enum gateway_discovery_assignment_delivery_kind {
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE = 0,
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM = 1,
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE = 2,
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_ABORT = 3,
};

struct gateway_discovery_assignment_state {
    struct proto_packet host_command;
    uint32_t result_reservation_token;
    uint64_t anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint64_t anchor_previous_hop_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t anchor_slots[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t anchor_hop_counts[UWB_DISCOVERY_SLOT_COUNT];
    uint64_t ack_mask;
    uint64_t claim_response_mask;
    uint64_t compact_observability_pending_mask;
    uint32_t epoch;
    uint32_t claim_command_seq;
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t table_command_seq;
    uint16_t table_packet_seq;
    uint32_t abort_command_seq;
    uint16_t abort_packet_seq;
    uint64_t operation_deadline_ms;
    uint64_t claim_collection_deadline_ms;
    uint64_t claim_ack_settle_deadline_ms;
    uint64_t response_ack_settle_deadline_ms;
    uint64_t response_window_origin_ms;
    uint64_t response_lane_start_ms;
    uint64_t phase_propagation_deadline_ms;
    uint32_t command_budget_ms;
    uint16_t response_spread_ms;
    uint32_t generation;
    uint32_t delivery_handle;
    uint32_t response_window_deadline_ms;
    size_t claim_count;
    enum gateway_discovery_assignment_stage stage;
    enum gateway_discovery_assignment_delivery_kind delivery_kind;
    uint8_t claim_round;
    uint8_t table_round;
    uint8_t claim_admission_retry_round;
    uint8_t table_admission_retry_round;
    uint8_t abort_admission_retry_count;
    uint8_t max_hop_count;
    uint8_t response_lane_max_hop_count;
    uint8_t prior_anchor_count;
    uint16_t expected_claim_count;
    uint16_t duplicate_count;
    enum command_status pending_failure_status;
    uint8_t pending_failure_reason;
    bool round_open;
    bool budget_explicit;
    bool claim_delivery_succeeded;
    bool claim_rf_started;
    bool table_delivery_succeeded;
    bool claim_ack_settle_armed;
    bool response_ack_settle_armed;
    bool response_window_deadline_valid;
    bool late_table_redrive_pending;
    bool table_delivery_is_redrive;
    bool ram_only_iteration;
    bool replay;
    bool response_lane_active;
    bool table_propagation_pending;
    bool survey_follows;
    bool active;
};
#endif

static struct anchor_discovery_claim_pending anchor_discovery_claim_pending;
static struct anchor_enumeration_response_config
    anchor_enumeration_response_config;
#if DEVICE_ROLE == ROLE_ANCHOR
/* The scanner is the sole mutator. This keeps ACK and child-record custody
 * across its short radio handoffs without retaining encoded RF frames. */
static struct enumeration_response_lane anchor_enumeration_response_lane;
#endif
static struct anchor_discovery_response_terminal
    anchor_discovery_response_terminal;
static uint32_t anchor_discovery_claim_failed_abandon_handle;
K_MUTEX_DEFINE(anchor_discovery_claim_mutex);
K_MUTEX_DEFINE(anchor_discovery_assignment_transaction_mutex);
#if DEVICE_ROLE == ROLE_GATEWAY
static struct gateway_discovery_assignment_state gateway_discovery_assignment_state;
K_MUTEX_DEFINE(gateway_discovery_assignment_mutex);
static struct app_discovery_assignment_work_guard
    gateway_discovery_assignment_publish_guard;
static uint32_t gateway_discovery_assignment_generation;
#endif

#if DEVICE_ROLE == ROLE_ANCHOR && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
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
#if DEVICE_ROLE == ROLE_GATEWAY
static struct app_gateway_operation_owner gateway_operation_owner;
static struct app_gateway_operation_lease gateway_assignment_operation_lease;
static struct app_gateway_operation_lease gateway_survey_operation_lease;
static struct k_spinlock gateway_operation_owner_lock;

static int gateway_operation_owner_claim(
    enum app_gateway_operation_owner_kind owner,
    struct app_gateway_operation_lease *lease)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_operation_owner_lock);
    int ret = app_gateway_operation_owner_claim(
        &gateway_operation_owner, owner, lease);

    k_spin_unlock(&gateway_operation_owner_lock, key);
    return ret;
}

static int gateway_operation_owner_release(
    struct app_gateway_operation_lease *lease)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_operation_owner_lock);
    int ret = app_gateway_operation_owner_release(
        &gateway_operation_owner, lease);

    if (ret == 0) {
        /*
         * The per-kind lease object is also the destination of the next
         * claim. Clear the retired capability before dropping the same lock
         * which serializes that claim, otherwise a successor can publish its
         * generation and have this delayed release erase it.
         */
        memset(lease, 0, sizeof(*lease));
    }
    k_spin_unlock(&gateway_operation_owner_lock, key);
    if (ret != 0) {
        status_debug_printf("DBG_GOWNER_RELEASE %u %u %d\n",
                            lease == NULL ? 0u :
                                (unsigned int)lease->owner,
                            lease == NULL ? 0u : lease->generation,
                            ret);
        app_watchdog_stop_feeding();
    }
    return ret;
}

static bool gateway_operation_owner_matches(
    enum app_gateway_operation_owner_kind owner,
    const struct app_gateway_operation_lease *lease)
{
    k_spinlock_key_t key = k_spin_lock(&gateway_operation_owner_lock);
    bool matches = lease != NULL &&
        app_gateway_operation_lease_valid(lease) &&
        lease->owner == owner &&
        gateway_operation_owner.active.owner == lease->owner &&
        gateway_operation_owner.active.generation == lease->generation;

    k_spin_unlock(&gateway_operation_owner_lock, key);
    return matches;
}

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
static uint32_t gateway_host_command_ack_confirm_wait_admission_id;
static uint32_t gateway_host_command_ack_confirm_wait_deadline_ms;
/*
 * Synchronous host-command dispatch has one authoritative reservation owner.
 * Any path that survives this dispatch must copy the token into its own
 * durable/asynchronous state before this value is cleared.
 */
static bool gateway_host_command_retry_pending;
static uint32_t gateway_host_command_next_admission_id;
static struct app_gateway_command_lifecycle gateway_host_command_lifecycle;
static struct k_spinlock gateway_host_command_lifecycle_lock;
BUILD_ASSERT(GATEWAY_HOST_COMMAND_QUEUE_DEPTH <=
             APP_GATEWAY_COMMAND_LIFECYCLE_MAX_ITEMS,
             "gateway command lifecycle must cover every queue slot");
#endif
static uint32_t gateway_route_refresh_result_token;
static uint16_t anchor_collection_result_seq;
K_MUTEX_DEFINE(anchor_command_result_mutex);
struct anchor_collection_result_pending {
    struct proto_packet command;
    struct command_result_id result_id;
    uint32_t collection_epoch_id;
    uint32_t delivery_handle;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool active;
    bool force_rediscovery_after_result;
    bool reboot_after_result;
    bool result_confirmed;
    uint8_t retry_round;
};
static struct anchor_collection_result_pending anchor_collection_result_pending;
struct anchor_pending_command_options {
    enum command_response_mode response_mode;
    uint32_t command_seq;
    uint32_t collection_epoch_id;
    uint32_t collection_slot_seed;
    uint32_t command_expiry_s;
    uint32_t execute_delay_ms;
    uint16_t expected_node_count;
    bool collection_required;
};
struct anchor_pending_command_execution {
    struct proto_packet command;
    struct anchor_pending_command_options options;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint32_t received_at_ms;
    uint32_t execute_at_ms;
    uint32_t execute_deadline_ms;
    bool side_effect_completed;
    bool terminal_without_execution;
    /* Independently arms the timestamps above; either may wrap to zero. */
    bool active;
};
static struct anchor_pending_command_execution anchor_pending_command_execution;
struct anchor_pending_click_handoff {
    struct uwb_wake_claim_frame claim;
    struct radio_guard_uwb_lease radio_lease;
    int64_t received_at_ms;
    uint8_t link_quality;
    bool active;
};
static struct anchor_pending_click_handoff anchor_pending_click_handoff;
static struct k_spinlock anchor_pending_click_handoff_lock;
static void anchor_set_uwb_busy(bool busy);
static void anchor_note_uwb_awake_since(int64_t start_ms, uint32_t already_counted_us);
static void anchor_operation_high_duty_boost_begin(uint32_t duration_ms);
static int anchor_enumeration_rx_begin(uint32_t epoch,
                                       uint32_t operation_budget_ms,
                                       bool allow_supersede);
static int anchor_enumeration_rx_prearm(uint32_t epoch,
                                        uint32_t hold_ms,
                                        uint32_t operation_budget_ms,
                                        bool survey_follows);
static int anchor_enumeration_rx_consume_survey_handoff(
    uint32_t assignment_epoch);
static int anchor_enumeration_rx_begin_table(
    uint32_t epoch,
    uint32_t operation_budget_ms,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    bool allow_supersede);
static bool anchor_enumeration_rx_bind_claim(uint32_t epoch,
                                             uint32_t claim_session_id,
                                             uint32_t claim_command_seq);
static bool anchor_enumeration_rx_active(void);
static bool anchor_enumeration_rx_matches_claim(
    uint32_t epoch,
    uint32_t claim_session_id,
    uint32_t claim_command_seq);
static bool anchor_enumeration_rx_matches_table(
    uint32_t epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment);
static enum protocol_rx_recovery_result anchor_enumeration_rx_note_recovery(
    bool recovered,
    const char *reason);
static bool anchor_enumeration_rx_terminate_claim(
    uint32_t epoch,
    uint32_t claim_session_id,
    uint32_t claim_command_seq,
    const char *reason);
static bool anchor_enumeration_rx_finish_table(
    uint32_t epoch,
    uint32_t table_command_seq,
    const struct discovery_assignment_table_commitment *table_commitment,
    const char *reason);
static int anchor_start_uwb_scan(void);
static void anchor_uwb_scan_work_handler(struct k_work *work);
static bool anchor_handle_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms);
static bool anchor_run_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms,
    struct radio_guard_uwb_lease *radio_lease);
static void anchor_click_handoff_work_handler(struct k_work *work);
int anchor_uwb_scan_schedule_ms(uint32_t delay_ms);
static void anchor_reboot_work_handler(struct k_work *work);
static void anchor_collection_result_work_handler(struct k_work *work);
static void anchor_command_execute_work_handler(struct k_work *work);
static int anchor_resume_pending_discovery_assignment_ack(
    bool explicit_table_replay);
static int anchor_schedule_reboot_after_command_result(void);
static int anchor_force_rediscovery_from_command(void);
#if DEVICE_ROLE == ROLE_GATEWAY
static int gateway_discovery_assignment_reschedule(k_timeout_t delay,
                                                    const char *source);
static int gateway_discovery_assignment_wake_now(const char *source);
#endif
static void anchor_heartbeat_work_handler(struct k_work *work);
static enum gateway_command_event_kind gateway_observability_kind(
    enum command_id command_id);
static struct gateway_command_event gateway_observability_event(
    enum gateway_command_event_kind kind,
    enum gateway_command_event_stage stage,
    enum command_id command_id,
    const struct proto_packet *host_command,
    uint32_t gateway_sequence);
static void gateway_route_refresh_observe(
    const struct app_node_comm_route_refresh_event *refresh);
static bool gateway_route_refresh_prearm_snapshot(uint32_t *epoch,
                                                   uint32_t *hold_ms,
                                                   struct operation_policy_set *policy,
                                                   bool *survey_follows);
static void gateway_observe_host_terminal(
    const struct proto_packet *host_command,
    enum command_id command_id,
    enum command_status status,
    enum gateway_command_event_reason reason);
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
static void gateway_observe_host_stage(
    const struct proto_packet *host_command,
    enum command_id command_id,
    enum gateway_command_event_stage stage);
static int gateway_observe_host_acceptance(
    const struct proto_packet *host_command,
    enum command_id command_id);
#endif


/* Implementation is split by responsibility but remains one translation unit. */
#include "app_anchor_commands.inc"
#include "app_anchor_gateway_control.inc"
#include "app_anchor_radio.inc"
#include "app_anchor_init.inc"
