#include "app_anchor.h"
#include "app_anchor_command_completion.h"
#include "app_anchor_ranging.h"
#include "app_anchor_survey_discovery.h"
#include "app_anchor_survey_result_delivery.h"
#include "app_anchor_survey_runtime.h"

#include "app_radio_low_power_policy.h"
#include "app_board.h"
#include "app_config.h"
#include "app_discovery_assignment_policy.h"
#include "app_discovery_assignment_stack.h"
#include "app_gateway_ble.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_survey_round.h"
#include "app_gateway_survey_observability.h"
#include "app_gateway_command_ingress.h"
#include "app_gateway_command_lifecycle.h"
#include "app_gateway_command_result.h"
#include "app_mesh_arbitration_zephyr.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_gateway_command_flow.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_persistence.h"
#include "app_mesh_report.h"
#include "app_mesh_route_state_persistence.h"
#include "app_mesh_rx_policy.h"
#include "app_ml.h"
#include "app_node_comm.h"
#include "app_node_comm_gateway_route_refresh.h"
#include "app_operation_policy.h"
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
#include "survey_round_control.h"
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
#define ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES 3u
#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS 60000u
#define ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS 3600000u
#define GATEWAY_HOST_COMMAND_QUEUE_DEPTH 2u
#define GATEWAY_HOST_ABORT_QUEUE_DEPTH 2u
#define GATEWAY_HOST_COMMAND_MAX_SEND_ATTEMPTS 8u
#define GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS DISCOVERY_ASSIGNMENT_DELIVERY_TERMINAL_POLL_MS
#define GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS \
    SURVEY_DISCOVERY_DELIVERY_TERMINAL_POLL_MS
#define GATEWAY_SURVEY_OPERATION_TERMINAL_SCHEDULING_GUARD_MS \
    (APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS + \
     APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS)
#define GATEWAY_SURVEY_TRANSACTION_POLL_MS 50u

BUILD_ASSERT(GATEWAY_SURVEY_TRANSACTION_POLL_MS <
             SURVEY_ROUND_GO_BASE_EXECUTE_DELAY_MS,
             "survey GO admission retries must fit before a fresh execution horizon");
BUILD_ASSERT(UWB_DISCOVERY_SLOT_COUNT == MESH_CONNECTED_MAX_ANCHORS,
             "gateway enumeration must cover the connected anchor maximum");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS >=
             DISCOVERY_ASSIGNMENT_OPERATION_MIN_BUDGET_MS,
             "default assignment budget must cover claim and table response horizons");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_TERMINAL_SCHEDULING_GUARD_MS >=
             DISCOVERY_ASSIGNMENT_CONTROL_PHASE_COUNT *
                 GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_POLL_MS,
             "assignment budget must cover every control terminal poll");
BUILD_ASSERT(GATEWAY_SURVEY_OPERATION_TERMINAL_SCHEDULING_GUARD_MS >=
                 GATEWAY_SURVEY_DISCOVERY_DELIVERY_POLL_MS,
             "survey budget must cover discovery flood terminal polling");
BUILD_ASSERT(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS <=
             GATEWAY_COMMAND_BUDGET_MAX_MS,
             "default assignment budget must fit the shared command budget limit");
BUILD_ASSERT((DISCOVERY_ASSIGNMENT_COMMAND_EXPIRY_S * 1000u) >=
             DISCOVERY_ASSIGNMENT_RESPONSE_CUSTODY_MAX_MS,
             "assignment command expiry must cover the maximum response custody horizon");
BUILD_ASSERT(SURVEY_GATEWAY_OPERATION_DEFAULT_BUDGET_MS <=
             GATEWAY_COMMAND_BUDGET_MAX_MS,
             "default survey budget must fit the shared command budget limit");
BUILD_ASSERT(SURVEY_GATEWAY_MAX_REPORTS == MESH_CONNECTED_MAX_ANCHORS,
             "gateway survey storage must cover the connected anchor maximum");
BUILD_ASSERT(SURVEY_DISCOVERY_MAX_SLOT_COUNT >= MESH_CONNECTED_MAX_ANCHORS,
             "survey discovery slots must cover every connected anchor");
BUILD_ASSERT(GATEWAY_COLLECTION_RESULT_CACHE_SIZE == MESH_CONNECTED_MAX_ANCHORS,
             "gateway collection storage must cover every connected anchor");
BUILD_ASSERT(APP_GATEWAY_ASSIGNMENT_PUBLISHER_MAX_ENTRIES ==
             MESH_CONNECTED_MAX_ANCHORS,
             "gateway assignment publication must cover every connected anchor");
BUILD_ASSERT(SURVEY_GATEWAY_MAX_PEERS_PER_REPORT == SURVEY_REACH_MAX_ENTRIES,
             "anchor collection and gateway survey report caps must match");
BUILD_ASSERT(SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT <= 16u,
             "survey result accounting uses one bounded 16-bit sample mask");
#if DEVICE_ROLE == ROLE_ANCHOR && !defined(CONFIG_IMEC_ML_ANCHOR)
BUILD_ASSERT(REPORT_TX_QUEUE_DEPTH >= SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
             "mesh anchor report custody must cover every admitted survey sample");
BUILD_ASSERT(APP_NODE_COMM_ORDINARY_DELIVERY_CAPACITY >=
                 SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT,
             "mesh anchor communication custody must admit a complete survey endpoint burst");
#endif
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
BUILD_ASSERT(ANCHOR_DISCOVERY_ACK_FAST_HANDLE_RETRIES > 0u &&
             ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS >
                 DISCOVERY_ASSIGNMENT_RETRY_MAX_MS &&
             ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_MAX_MS >=
                 ANCHOR_DISCOVERY_ACK_LOW_DUTY_RETRY_BASE_MS,
             "assignment ACK low-duty retry must be bounded and slower than fast recovery");

#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(UWB_RANGE_SCHEDULE_MAX_LEN <= UWB_MESH_MAX_FRAME_LEN,
             "post-wake route RX buffer must still fit normal ranging schedules");
BUILD_ASSERT(ANCHOR_UWB_SCAN_WORKQUEUE_STACK_SIZE >= 12288u,
             "mesh-route anchor scan needs the enlarged wake-frame stack");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_PRIORITY < ANCHOR_UWB_SCAN_WORKQUEUE_PRIORITY,
             "mesh route work must preempt low-duty anchor scan handoff");
BUILD_ASSERT(MESH_ROUTE_WORKQUEUE_STACK_SIZE >= 9216u,
             "mesh communication worker must retain its verified route stack budget");
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
    uint64_t absolute_deadline_ms;
    uint64_t next_attempt_not_before_ms;
    uint16_t response_spread_ms;
    enum discovery_assignment_phase phase;
    uint8_t slot;
    uint8_t slot_count;
    uint8_t hop_count;
    uint8_t attempt;
    uint8_t terminal_retry_count;
    bool ack_delivered;
    bool active;
};

#if DEVICE_ROLE == ROLE_GATEWAY
enum gateway_discovery_assignment_stage {
    GATEWAY_DISCOVERY_ASSIGNMENT_COLLECT_CLAIMS = 0,
    GATEWAY_DISCOVERY_ASSIGNMENT_WAIT_TABLE_ACKS = 1,
};

enum gateway_discovery_assignment_delivery_kind {
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_NONE = 0,
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_CLAIM = 1,
    GATEWAY_DISCOVERY_ASSIGNMENT_DELIVERY_TABLE = 2,
};

struct gateway_discovery_assignment_state {
    struct proto_packet host_command;
    uint32_t result_reservation_token;
    uint64_t anchor_ids[UWB_DISCOVERY_SLOT_COUNT];
    uint8_t anchor_slots[UWB_DISCOVERY_SLOT_COUNT];
    uint64_t ack_mask;
    uint64_t claim_response_mask;
    uint32_t epoch;
    uint32_t claim_command_seq;
    struct discovery_assignment_table_commitment table_commitment;
    uint32_t table_command_seq;
    uint16_t table_packet_seq;
    uint64_t operation_deadline_ms;
    uint64_t claim_collection_deadline_ms;
    uint64_t claim_ack_settle_deadline_ms;
    uint64_t response_ack_settle_deadline_ms;
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
    uint8_t max_hop_count;
    uint8_t prior_anchor_count;
    uint16_t expected_claim_count;
    uint16_t duplicate_count;
    bool round_open;
    bool budget_explicit;
    bool claim_delivery_succeeded;
    bool table_delivery_succeeded;
    bool claim_ack_settle_armed;
    bool response_ack_settle_armed;
    bool response_window_deadline_valid;
    bool late_table_redrive_pending;
    bool active;
};
#endif

static struct anchor_discovery_claim_pending anchor_discovery_claim_pending;
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
static struct survey_gateway_context gateway_survey_context;
static bool gateway_survey_active;
static uint32_t gateway_survey_operation_deadline_ms;
static uint32_t gateway_survey_discovery_delivery_handle;
static uint32_t gateway_survey_collection_deadline_ms;
static uint32_t gateway_survey_collection_emission_deadline_ms;
static uint32_t gateway_survey_collection_duration_ms;
static uint16_t gateway_survey_expected_node_count;
static uint8_t gateway_survey_max_pair_reruns =
    OPERATION_POLICY_PAIR_DEFAULT_MAX_RERUNS;
static uint8_t gateway_survey_max_parallel_pairs =
    OPERATION_POLICY_PAIR_DEFAULT_MAX_PARALLEL_PAIRS;
static bool gateway_survey_collection_window_armed;
static bool gateway_survey_collection_pending;
static bool gateway_survey_expected_node_count_present;
static struct k_work_delayable gateway_survey_work;
static struct survey_gateway_auto_context gateway_survey_auto;
static struct proto_packet gateway_survey_pending_command;
static bool gateway_survey_pending_command_valid;
static struct proto_packet gateway_survey_host_command;
static uint16_t gateway_survey_duplicate_count;
static uint16_t gateway_survey_pair_success_count;
static uint16_t gateway_survey_pair_failure_count;
static uint16_t gateway_survey_discovery_failure_count;
static bool gateway_survey_topology_accounted;
static enum gateway_command_event_reason gateway_survey_terminal_failure_reason;
static uint16_t gateway_survey_pair_result_mask;
static uint16_t gateway_survey_pair_responder_usable_mask;
static uint16_t gateway_survey_pair_initiator_unusable_mask;
static uint16_t gateway_survey_pair_responder_unusable_mask;
static struct survey_sample_observation_identity
    gateway_survey_pair_initiator_identities
    [SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT];
static struct survey_sample_observation_identity
    gateway_survey_pair_responder_identities
    [SURVEY_PAIR_RUNTIME_MAX_SAMPLE_COUNT];
static bool gateway_survey_pair_observation_active;
static struct survey_gateway_response_ack_settle
    gateway_survey_response_ack_settle;
static struct app_gateway_survey_observability_state
    gateway_survey_observability;
struct gateway_survey_sequential_run {
    struct survey_pair pair;
    uint16_t round_id;
    uint16_t generation_cursor;
    uint8_t reruns_started;
    bool valid;
};
static struct gateway_survey_sequential_run gateway_survey_sequential_run;
#if DEVICE_ROLE == ROLE_GATEWAY
struct gateway_survey_cleanup_delivery {
    struct survey_pair pair;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint64_t target_id;
    uint64_t next_hop_id;
    uint64_t absolute_deadline_ms;
    uint32_t client_token;
    uint32_t handle;
    uint16_t sequence;
    uint16_t round_id;
    uint8_t hop_count;
    uint8_t peer_mask;
    bool active;
    bool prepared;
    bool submitted;
    bool completion_ready;
    bool peer_unavailable;
};
struct gateway_survey_result_preflight {
    struct node_transaction_key key;
    enum survey_gateway_transaction_result result;
    enum command_status status;
    uint8_t reason;
    bool valid;
};
struct gateway_survey_result_preparation {
    struct node_transaction_key key;
    enum survey_gateway_transaction_result result;
    uint64_t received_at_ms;
    uint8_t result_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t result_token;
    enum command_id command_id;
    enum command_status status;
    uint8_t reason;
    bool valid;
};
static struct survey_gateway_transaction gateway_survey_transaction;
static struct app_gateway_survey_round gateway_survey_round;
static struct gateway_survey_cleanup_delivery gateway_survey_cleanup;
static struct gateway_survey_result_preflight gateway_survey_result_preflight;
static struct gateway_survey_result_preparation
    gateway_survey_result_preparation;
struct gateway_manual_survey_pair_state {
    struct survey_pair pair;
    uint8_t round_commitment[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t deadline_ms;
    uint32_t go_due_ms;
    uint32_t go_delivery_handle;
    uint16_t round_id;
    uint8_t prepare_submitted_mask;
    uint8_t prepare_possible_mask;
    uint8_t prepared_mask;
    uint8_t start_submitted_mask;
    uint8_t started_mask;
    uint8_t abort_submitted_mask;
    uint8_t aborted_mask;
    uint16_t initiator_result_mask;
    uint16_t responder_result_mask;
    uint8_t valid : 1;
    uint8_t go_rf_started : 1;
    uint8_t cleanup_requested : 1;
};
static struct gateway_manual_survey_pair_state
    gateway_manual_survey_pair_state;
enum gateway_operation_owner {
    GATEWAY_OPERATION_OWNER_NONE = 0,
    GATEWAY_OPERATION_OWNER_AUTO_SURVEY,
    GATEWAY_OPERATION_OWNER_MANUAL_SURVEY,
    GATEWAY_OPERATION_OWNER_ASSIGNMENT,
};
static atomic_t gateway_operation_owner =
    ATOMIC_INIT(GATEWAY_OPERATION_OWNER_NONE);

static bool gateway_operation_owner_claim(enum gateway_operation_owner owner)
{
    return owner != GATEWAY_OPERATION_OWNER_NONE &&
           atomic_cas(&gateway_operation_owner,
                      GATEWAY_OPERATION_OWNER_NONE,
                      (atomic_val_t)owner);
}

static void gateway_operation_owner_release(
    enum gateway_operation_owner owner)
{
    if (owner != GATEWAY_OPERATION_OWNER_NONE) {
        (void)atomic_cas(&gateway_operation_owner,
                         (atomic_val_t)owner,
                         GATEWAY_OPERATION_OWNER_NONE);
    }
}

static uint32_t gateway_survey_transaction_client_token;
static enum command_status gateway_survey_finish_pending_status;
static enum gateway_command_event_reason gateway_survey_finish_pending_reason;
static bool gateway_survey_finish_pending;
static uint32_t gateway_survey_round_go_delivery_handle;
static uint32_t gateway_survey_round_observation_deadline_ms;
static size_t gateway_survey_round_cleanup_lane_index;
static bool gateway_survey_round_cleanup_lane_valid;
#endif
#if DEVICE_ROLE == ROLE_GATEWAY && defined(CONFIG_IMEC_GATEWAY_BLE)
struct gateway_host_abort_item {
    struct proto_packet packet;
    uint32_t result_reservation_token;
    enum command_id command_id;
};

BUILD_ASSERT(sizeof(struct gateway_host_abort_item) <= 48u,
             "local survey-abort custody must not allocate a generic payload");

K_MSGQ_DEFINE(gateway_host_command_msgq,
              sizeof(struct app_gateway_command_ingress_item),
              GATEWAY_HOST_COMMAND_QUEUE_DEPTH,
              4);
K_MSGQ_DEFINE(gateway_host_abort_msgq,
              sizeof(struct gateway_host_abort_item),
              GATEWAY_HOST_ABORT_QUEUE_DEPTH,
              4);
static struct k_work_delayable gateway_host_abort_work;
static struct k_work_delayable gateway_host_abort_route_work;
static struct k_work_delayable gateway_host_command_work;
static struct k_work_delayable gateway_host_command_retry_work;
static uint8_t gateway_host_command_retry_round;
static uint32_t gateway_host_command_retry_started_ms;
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
BUILD_ASSERT(GATEWAY_HOST_COMMAND_QUEUE_DEPTH +
                 GATEWAY_HOST_ABORT_QUEUE_DEPTH + 1u ==
             APP_GATEWAY_COMMAND_RESULT_QUEUE_DEPTH,
             "result custody must cover normal, queued abort, and active abort commands");
#endif
static uint32_t gateway_route_refresh_result_token;
static uint32_t anchor_collection_node_boot_counter;
static uint16_t anchor_collection_result_seq;
static bool anchor_collection_result_restore_complete;
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
/*
 * Survey GO owns the tight synchronized execution lane. One ordinary delayed
 * broadcast may be parked here while GO executes, then resumes against its
 * original absolute deadline.
 */
static struct anchor_pending_command_execution
    anchor_deferred_command_execution;
struct anchor_pending_click_handoff {
    struct uwb_wake_claim_frame claim;
    int64_t received_at_ms;
    uint8_t link_quality;
    bool active;
};
static struct anchor_pending_click_handoff anchor_pending_click_handoff;
static struct k_spinlock anchor_pending_click_handoff_lock;
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
static bool anchor_run_mesh_click_wake_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t link_quality,
    int64_t received_at_ms);
static void anchor_click_handoff_work_handler(struct k_work *work);
static int anchor_uwb_scan_schedule_ms(uint32_t delay_ms);
static void anchor_reboot_work_handler(struct k_work *work);
static void anchor_collection_result_work_handler(struct k_work *work);
static void anchor_command_execute_work_handler(struct k_work *work);
static int anchor_resume_pending_discovery_assignment_ack(
    bool explicit_table_replay);
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
#if DEVICE_ROLE == ROLE_GATEWAY
static bool gateway_survey_round_active(void);
static bool gateway_survey_round_drive(void);
static int gateway_survey_round_note_sample(uint64_t reporter_id,
                                            const struct survey_sample *sample,
                                            uint64_t received_at_ms);
static bool gateway_survey_round_note_control_result(
    const struct proto_packet *command,
    enum command_id command_id,
    enum command_status status,
    uint8_t reason);
static int gateway_survey_round_commitment(
    uint8_t commitment[SEMANTIC_DIGEST_SHA256_LEN]);
static void gateway_survey_round_fail_current_control(
    enum command_id command_id,
    uint64_t target_id,
    enum gateway_command_event_reason failure_reason);
static void gateway_survey_round_note_cleanup_peer(uint8_t peer_mask);
static void gateway_survey_round_reset(void);
static void gateway_manual_survey_pair_reset(void);
#endif
static void anchor_heartbeat_work_handler(struct k_work *work);


/* Implementation is split by responsibility but remains one translation unit. */
#include "app_anchor_commands.inc"
#include "app_anchor_gateway_survey.inc"
#include "app_anchor_gateway_survey_round.inc"
#include "app_anchor_gateway_control.inc"
#include "app_anchor_radio.inc"
#include "app_anchor_init.inc"
