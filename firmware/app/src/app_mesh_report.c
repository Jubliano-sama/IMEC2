#include "app_mesh_report.h"
#include "app_mesh_report_encode.h"

#include "app_board.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_direct_probe_diag.h"
#include "app_mesh_direct_gateway_retry.h"
#include "app_mesh_event_retry.h"
#include "app_mesh_coordinator.h"
#include "app_mesh_coordinator_runtime.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_flood.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_collection_deferral.h"
#include "app_mesh_gateway_ack_policy.h"
#include "app_mesh_persistence.h"
#include "app_mesh_preemption.h"
#include "app_mesh_route_reply_ack.h"
#include "app_mesh_route_ready_handoff.h"
#include "app_mesh_route_request_policy.h"
#include "app_mesh_route_wait_tx.h"
#include "app_mesh_rf_retry.h"
#include "app_mesh_result_handoff.h"
#include "app_mesh_rx_policy.h"
#include "app_mesh_test.h"
#include "app_mesh_tx_handoff_gate.h"
#include "app_node_comm.h"
#include "app_node_comm_gateway_control.h"
#include "app_node_comm_gateway_route_refresh.h"
#include "app_state.h"
#include "app_stack_workload_diag.h"
#include "app_wake_train_politeness.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "discovery_assignment.h"
#include "mesh.h"
#include "mesh_preemption.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "route.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_report, LOG_LEVEL_DBG);

#define MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS 50u
#define MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_COUNT 2u
#define MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS 25u
#define MESH_ROUTE_TEST_ROUTE_REPLY_RX_GUARD_MS 20u
#define MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS \
    MESH_RADIO_EVENT_ACCEPT_DELAY_MS
#define MESH_ROUTE_TEST_REPLY_HANDOFF_WAIT_MS 250u
#define MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS 1000u
#define RREP_ACK_MAX_C5_PREEMPTIONS 2u
#define RREP_ACK_ATTEMPT_MAX_MS \
    (RREP_ACK_TIMEOUT_MS * (RREP_ACK_MAX_C5_PREEMPTIONS + 1u))
#define MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS \
    ((uint32_t)(RREP_RETRY_COUNT_PER_HOP + 1u) * \
     (RREP_ACK_ATTEMPT_MAX_MS + MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS + \
      MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS + 50u))
#define MESH_ROUTE_TEST_REPLY_CAPTURE_MAX 4u
#define MESH_EVENT_CONTROL_COMPACT_PAYLOAD_MAX 64u
#define MESH_EVENT_PROPOSE_RETRY_DEADLINE_MS 6000u
#define MESH_EVENT_ACCEPT_RETRY_DEADLINE_MS MESH_EVENT_PROPOSE_RETRY_DEADLINE_MS
#define MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS 5u
#define MESH_ROUTE_TEST_ROUTE_ADV_REPLY_GUARD_MS 20u
#define MESH_ROUTE_TEST_EMBEDDED_ROUTE_SUPPRESS_MS 1000u
#define MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS 50u
#define MESH_ROUTE_WAIT_RX_SUPPRESS_MS 100u
#define MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS \
    MESH_ROUTE_TEST_ROUTE_REPLY_RX_GUARD_MS
#define MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS 1000u
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
#define MESH_CH9_TX_BATCH_MAX 4u
#else
#define MESH_CH9_TX_BATCH_MAX 8u
#endif
#define MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP 128u
#define MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS 60000u
#define MESH_ROUTE_EXHAUSTED_RETRY_JITTER_MS 30000u

BUILD_ASSERT(MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS +
             MESH_ROUTE_TEST_ROUTE_ADV_REPLY_GUARD_MS <
             MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
             "route-ad response delay must fit the route reply listen window");
BUILD_ASSERT(MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP <= UWB_MESH_MAX_PAYLOAD_LEN,
             "direct gateway ACK scratch must fit the mesh payload limit");
BUILD_ASSERT(MESH_EVENT_CONTROL_COMPACT_PAYLOAD_MAX <= UWB_MESH_MAX_PAYLOAD_LEN,
             "event-control retry snapshot must fit the mesh payload limit");
BUILD_ASSERT(MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS >= ROUTE_GATEWAY_ACK_TIMEOUT_MS,
             "exhausted route retry must be slower than one ACK timeout");
#define MESH_CH9_DATA_RATE_BPS 850000u
#define MESH_CH9_PHY_OVERHEAD_US 1500u
#define MESH_CH9_TX_FRAME_GAP_MS 2u
#define MESH_GATEWAY_RX_REARM_GUARD_MS \
    APP_MESH_DIRECT_GATEWAY_SURVEY_SERVICE_GUARD_MS
#define MESH_CH9_DIRECT_GATEWAY_TX_GAP_SLOP_MS 5u
#define MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS 25u
#define MESH_CH9_TX_CONFIG_GUARD_MS 25u
#define MESH_CH9_TX_SLOT_TRAILER_MS 5u
#define MESH_ROUTE_TEST_CH9_TX_OFFSET_MS MESH_RADIO_EVENT_TX_OFFSET_MS
#define MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS 30u
#define MESH_EVENT_CONTROL_CH5_AIRTIME_MS \
    MESH_RADIO_EVENT_CONTROL_REFERENCE_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_GUARD_MS \
    APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS \
    APP_MESH_DIRECT_GATEWAY_ACK_RX_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_SLICE_MS 25u
#define MESH_GATEWAY_DIRECT_PROBE_ATTEMPTS \
    APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS
#define MESH_GATEWAY_DIRECT_PROBE_BACKOFF_MIN_MS \
    APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MIN_MS
#define MESH_GATEWAY_DIRECT_PROBE_BACKOFF_MAX_MS \
    APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_MAX_MS
#define MESH_GATEWAY_DIRECT_PROBE_ATTEMPT_MS \
    (UWB_MESH_TX_TIMEOUT_MS + MESH_GATEWAY_DIRECT_PROBE_ACK_GUARD_MS + \
     MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS)
#define MESH_GATEWAY_DIRECT_PROBE_ROUND_MS \
    ((MESH_GATEWAY_DIRECT_PROBE_ATTEMPTS * MESH_GATEWAY_DIRECT_PROBE_ATTEMPT_MS) + \
     ((MESH_GATEWAY_DIRECT_PROBE_ATTEMPTS - 1u) * \
      MESH_GATEWAY_DIRECT_PROBE_BACKOFF_MAX_MS) + 50u)
#define MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS 10u
#define MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS \
    (MESH_CH9_TX_CONFIG_GUARD_MS + MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS + \
     MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS)
#define MESH_DIRECT_GATEWAY_BATCH_TX_WINDOW_MS MESH_EVENT_DEFAULT_WINDOW_MS
#define MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS \
    (MESH_DIRECT_GATEWAY_BATCH_TX_WINDOW_MS + \
     MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS)
#define MESH_CH9_BATCH_FLAG_FINAL 0x01u
#define MESH_ROUTE_TEST_CH5_GAP_SCAN_MS 100u
#define MESH_GATEWAY_CH5_CONTINUOUS_RX_MS 2000u
#define MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS 20u
#define MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS
#define MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS 2u
#define MESH_GATEWAY_ROUTE_PREEMPT_MS 2000u
#define MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS 10u
#define MESH_CONTROL_RX_HANDOFF_TIMEOUT_MS 25u
#define MESH_GATEWAY_ROUTE_ADV_DEFER_MS 1000u
#define MESH_C5_FLOOD_DELAY_LISTEN_SLICE_MS 80u
#define MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES 55u
#define MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN 125u
#define MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS 20u
#define MESH_ROUTE_WAKE_MAX_TX_PER_RX_CHECK 2u
#define MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS \
    (MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS + \
     (MESH_ROUTE_WAKE_MAX_TX_PER_RX_CHECK * UWB_CONTROL_TX_TIMEOUT_MS) + \
     ((UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US + 999u) / 1000u))
#define MESH_ROUTE_WAKE_ROUTE_MAGIC0 0x4du
#define MESH_ROUTE_WAKE_ROUTE_MAGIC1 0x52u
#define MESH_ROUTE_WAKE_ROUTE_VERSION 1u
#define MESH_ROUTE_WAKE_ROUTE_HEADER_LEN 13u
#define MESH_ROUTE_WAKE_ROUTE_CRC_LEN 2u
#define MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN \
    (MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES + \
     MESH_ROUTE_WAKE_ROUTE_CRC_LEN)
#define MESH_ROUTE_REPLY_LISTEN_FIXED_MS \
    (MAX(MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS, \
         MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS) + \
     RREP_RESPONDER_JITTER_MAX_MS + MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS + \
     MESH_ROUTE_TEST_REPLY_WINDOW_GUARD_MS)
#define MESH_ROUTE_REPLY_LISTEN_PER_FORWARD_HOP_MS \
    (WAKE_ADV_MS + \
     MAX(MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS, \
         MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS) + \
     FLOOD_RELAY_BURST_MS + FLOOD_WAVE_MS + \
     MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS + \
     MESH_ROUTE_TEST_REPLY_WINDOW_GUARD_MS)
#define MESH_ROUTE_REPLY_LISTEN_WORST_CASE_MS \
    (MESH_ROUTE_REPLY_LISTEN_FIXED_MS + \
     ((FLOOD_EPOCH_CRITICAL_TTL - 1u) * \
      MESH_ROUTE_REPLY_LISTEN_PER_FORWARD_HOP_MS))

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(MESH_RELAY_EVENT_TIMINGS >= MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
             "mesh route-test needs room for upstream and downstream channel-9 timings");
BUILD_ASSERT(MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS <= UINT16_MAX,
             "mesh route-test route-reply ETA must fit in the uint16_t TLV");
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_TX_OFFSET_MS + MESH_CH9_TX_SLOT_TRAILER_MS <
             MESH_EVENT_DEFAULT_WINDOW_MS,
             "mesh route-test TX offset must fit inside the channel-9 window");
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS >= MESH_EVENT_DEFAULT_GUARD_MS,
             "mesh route-test retune guard must cover the negotiated event guard");
BUILD_ASSERT(MESH_EVENT_DEFAULT_WINDOW_MS + MESH_EVENT_RX_LATE_GUARD_MS <
             MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS,
             "mesh route-test late RX guard must not overlap the second channel-9 slot");
BUILD_ASSERT(MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS < ROUTE_GATEWAY_ACK_TIMEOUT_MS,
             "direct gateway batch window must fit inside the ACK timeout");
BUILD_ASSERT((MESH_EVENT_DEFAULT_MAX_MISSED *
              MESH_EVENT_DEFAULT_INTERVAL_MS * 2u) >
             (ROUTE_GATEWAY_ACK_TIMEOUT_MS + MESH_RELAY_RETRY_BACKOFF_MAX_MS),
             "channel-9 missed-RX budget must cover the longest ACK retry gap");
BUILD_ASSERT(MESH_EVENT_DEFAULT_SUPERVISION_MS >
             MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS,
             "channel-9 supervision must cover all permitted ACK retries");
BUILD_ASSERT(MESH_GATEWAY_DIRECT_PROBE_BACKOFF_MAX_MS >=
             MESH_GATEWAY_DIRECT_PROBE_BACKOFF_MIN_MS,
             "direct gateway probe retry backoff range must be ordered");
BUILD_ASSERT(MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS >= MESH_GATEWAY_RX_REARM_GUARD_MS,
             "direct gateway batches must leave time for gateway RX re-arm");
BUILD_ASSERT(UWB_MESH_GATEWAY_RX_WINDOW_MS < APP_WATCHDOG_PROGRESS_LEASE_MS,
             "gateway continuous RX window must fit the watchdog progress lease");
BUILD_ASSERT(MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS >=
             MESH_GATEWAY_RX_REARM_GUARD_MS +
             MESH_CH9_DIRECT_GATEWAY_TX_GAP_SLOP_MS,
             "direct gateway batch gap must tolerate sender-side TX timing slop");
BUILD_ASSERT(MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS +
             MESH_CH9_TX_CONFIG_GUARD_MS < MESH_DIRECT_GATEWAY_BATCH_TX_WINDOW_MS,
             "direct gateway batch spacing must still leave payload TX time");
BUILD_ASSERT(UWB_WAKE_CLAIM_LEN + MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN <=
             MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
             "mesh route-test compact wake route request must fit standard channel-5 PHR");
BUILD_ASSERT(MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS < WAKE_ADV_MS,
             "route wake TX gaps must leave a click receive opportunity inside one wake train");
BUILD_ASSERT(MESH_ROUTE_REPLY_LISTEN_WORST_CASE_MS <
             APP_WATCHDOG_PROGRESS_LEASE_MS,
             "worst-case route reply listen must fit inside the watchdog progress lease");
BUILD_ASSERT(APP_MESH_CH9_ACK_BATCH_ENTRY_MAX >= MESH_CH9_TX_BATCH_MAX,
             "mesh route-test ACK batch must cover the largest TX batch");
#if DEVICE_ROLE == ROLE_ANCHOR
BUILD_ASSERT(MESH_CH9_TX_BATCH_MAX <= REPORT_TX_QUEUE_DEPTH,
             "mesh route-test TX batch must fit in the report TX queue");
#endif
#endif

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint64_t previous_hop_id;
    uint8_t link_quality;
    uint8_t radio_channel;
    uint32_t received_at_ms;
    bool current_channel9_plan_valid;
    struct mesh_event_plan current_channel9_plan;
};

struct mesh_frame_parse_context {
    bool found;
    uint64_t previous_hop_id;
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    size_t payload_len;
};

struct mesh_reply_capture {
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    size_t frame_len;
    uint8_t quality;
    uint32_t received_at_ms;
};

struct mesh_route_embedded_rx_state {
    bool valid;
    struct proto_packet packet;
    uint32_t received_at_ms;
};

struct mesh_event_control_record {
    struct proto_packet packet;
    struct mesh_event_timing timing;
    uint8_t payload[MESH_EVENT_CONTROL_COMPACT_PAYLOAD_MAX];
    uint64_t peer_id;
    uint32_t encoded_delay_ms;
    uint8_t payload_len;
    bool valid;
};

struct mesh_event_accept_retry_context {
    struct mesh_event_control_record response;
    struct app_mesh_event_retry_state retry;
    struct mesh_event_timing reservation_timing;
    struct mesh_event_timing previous_timing;
    bool had_previous_timing;
    bool replay_existing_response;
};

struct mesh_event_accept_completed {
    struct mesh_event_control_record response;
    struct app_mesh_event_completion completion;
    uint16_t retry_round;
};

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet);
static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out);
static uint32_t mesh_ch9_estimated_airtime_ms(size_t frame_len);
static void report_tx_schedule_backoff(uint32_t delay_ms, const char *reason);

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
#if DEVICE_ROLE == ROLE_ANCHOR
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);
#endif
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_uwb_rx_work;
static struct k_work mesh_uwb_rx_rearm_work;
static struct k_work_delayable mesh_persistence_retry_work;
static bool mesh_outbox_persistence_dirty;
static bool mesh_child_custody_persistence_dirty;
static uint8_t mesh_persistence_retry_round;
static struct mesh_delivery_health mesh_delivery_health;
static struct k_work_delayable mesh_tx_timeout_work;
#if DEVICE_ROLE == ROLE_ANCHOR
static struct k_work_delayable report_tx_work;
static struct k_work_delayable mesh_route_request_action_work;
#endif
static struct k_work_delayable mesh_c5_flood_work;
static struct k_work_delayable mesh_route_discovery_work;
static struct k_work_delayable mesh_event_negotiation_retry_work;
static K_MUTEX_DEFINE(mesh_uwb_rx_rearm_lock);
static uint32_t mesh_uwb_rx_rearm_delay_ms;
static bool mesh_uwb_rx_rearm_pending;
static struct app_mesh_rf_retry_state mesh_route_request_wake_rf_retry;
static struct app_mesh_rf_retry_state mesh_route_request_control_rf_retry;
static struct app_mesh_rf_retry_state mesh_retransmit_rf_retry;
static struct app_mesh_rf_retry_state mesh_deferred_gateway_ack_rf_retry;
static struct app_mesh_rf_retry_state mesh_route_wait_delivery_rf_retry;
#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t report_tx_backoff_until_ms;
static uint32_t report_tx_retry_delay_override_ms;
static bool report_tx_retry_delay_override_valid;
static struct app_mesh_rf_retry_state
    mesh_report_rf_retry_states[REPORT_TX_QUEUE_DEPTH];
static struct app_mesh_rf_retry_bank mesh_report_rf_retry_bank = {
    .states = mesh_report_rf_retry_states,
    .state_count = ARRAY_SIZE(mesh_report_rf_retry_states),
};
#endif
static atomic_t mesh_rx_response_active_state;
static atomic_t mesh_rx_handler_active_state;
static atomic_t mesh_transport_paused_state;
static K_MUTEX_DEFINE(mesh_rx_handler_lock);
static const char *mesh_rx_handler_lock_owner;
static uint32_t mesh_rx_handler_lock_since_ms;
static uint32_t mesh_rx_window_log_next_ms;
static uint32_t mesh_anchor_rx_yield_log_next_ms;
static uint32_t gateway_rx_diag_next_ms;
static bool mesh_route_reply_handoff_pending;
static uint32_t mesh_route_reply_handoff_deadline_ms;
struct app_mesh_command_orchestrator *mesh_gateway_command_orchestrator_context(void)
{
    return app_node_comm_gateway_control_context();
}

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
K_THREAD_STACK_DEFINE(mesh_route_work_q_stack, MESH_ROUTE_WORKQUEUE_STACK_SIZE);
static struct k_work_q mesh_route_work_q;
static const struct k_work_queue_config mesh_route_work_q_config = {
    .name = "mesh_route",
};
#endif

static const struct app_mesh_report_callbacks *mesh_report_callbacks;

static struct app_mesh_ch9_ack_table mesh_ch9_ack_table;

struct mesh_ch9_tx_pending_entry {
    struct mesh_outbound outbound;
    uint32_t packet_id;
    bool has_packet_id;
    bool acked;
};

struct mesh_ch9_tx_pending_batch {
    struct mesh_ch9_tx_pending_entry entries[MESH_CH9_TX_BATCH_MAX];
    uint8_t count;
    uint64_t next_hop_id;
    uint32_t deadline_ms;
    bool active;
};

static struct mesh_ch9_tx_pending_batch mesh_ch9_tx_pending;
struct mesh_ch9_batch_metadata {
    uint32_t batch_id;
    uint8_t flags;
    bool present;
    bool final_packet;
};

static K_MUTEX_DEFINE(mesh_ch9_batch_payload_lock);
static K_MUTEX_DEFINE(mesh_send_scratch_lock);
static struct mesh_outbound mesh_send_scratch_tx;
static uint8_t mesh_send_scratch_frame[UWB_MESH_MAX_FRAME_LEN];
static uint8_t mesh_ch9_batch_payload_scratch[UWB_MESH_MAX_PAYLOAD_LEN];
#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t mesh_ch9_batch_next_id;

struct mesh_ch9_slot_tx_context {
    int64_t uwb_window_start_ms;
    bool active;
};
#endif

enum mesh_radio_release_policy {
    MESH_RADIO_RELEASE_STANDBY,
    MESH_RADIO_RELEASE_IDLE,
};

#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_outbound report_tx_worker_scratch;
static struct mesh_outbound report_tx_batch_candidates[MESH_CH9_TX_BATCH_MAX];
static struct mesh_outbound report_tx_queue_overflow_dropped;
static bool report_tx_queue_recovery_valid;
#endif
static struct mesh_relay_result mesh_work_result;
static struct mesh_outbound mesh_tx_timeout_pending_waiting;
#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_outbound mesh_tx_timeout_pending_report;
#endif
static struct mesh_rx_pending mesh_rx_work_pending;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static struct proto_packet mesh_direct_gateway_ack_packet;
static uint8_t mesh_direct_gateway_ack_payload[MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP];
static K_MUTEX_DEFINE(mesh_direct_gateway_probe_scratch_lock);
static struct mesh_outbound mesh_direct_gateway_probe_scratch;
#if DEVICE_ROLE == ROLE_ANCHOR
static K_MUTEX_DEFINE(mesh_route_reply_ack_scratch_lock);
static struct mesh_frame_parse_context mesh_route_reply_ack_parsed;
static uint8_t mesh_route_reply_ack_frame[UWB_MESH_MAX_FRAME_LEN];
#endif
static K_MUTEX_DEFINE(mesh_route_wake_scratch_lock);
static struct uwb_clicker_session mesh_route_wake_session_scratch;
static struct uwb_clicker_config mesh_route_wake_config_scratch;
static uint8_t mesh_route_wake_suffix_scratch[MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN];
static uint8_t mesh_route_wake_frame_scratch[MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN];
static K_MUTEX_DEFINE(mesh_route_wait_scratch_lock);
static struct mesh_outbound mesh_route_waiting_tx_scratch;
static struct mesh_outbound mesh_deferred_gateway_ack_scratch;
static struct app_mesh_paused_delivery_state mesh_paused_delivery;
#if DEVICE_ROLE == ROLE_ANCHOR
static K_MUTEX_DEFINE(report_tx_queue_overflow_lock);
#endif
static K_MUTEX_DEFINE(mesh_c5_control_scratch_lock);
static struct k_spinlock mesh_rx_handoff_lock;
static struct app_mesh_rx_handoff_state mesh_rx_handoff;
#if DEVICE_ROLE == ROLE_ANCHOR
static K_MUTEX_DEFINE(mesh_route_reply_scratch_lock);
static struct mesh_outbound mesh_route_reply_backup_scratch;
static K_MUTEX_DEFINE(mesh_route_request_action_scratch_lock);
static struct mesh_outbound mesh_route_request_action_tx;
static struct mesh_outbound mesh_route_request_reply_tx;
static uint64_t mesh_route_request_action_previous_hop_id;
static uint32_t mesh_route_request_action_reply_deadline_ms;
static bool mesh_route_request_action_pending;
#endif
static struct mesh_outbound mesh_result_action_tx;
static K_MUTEX_DEFINE(mesh_route_discovery_lock);
static uint64_t mesh_route_discovery_target_id;
static const char *mesh_route_discovery_reason;
static bool mesh_route_discovery_pending;
static uint64_t mesh_route_ready_event_peer_id;
static struct mesh_event_control_record mesh_event_propose_record;
static struct app_mesh_event_retry_state mesh_event_propose_retry;
static struct mesh_event_accept_retry_context mesh_event_accept_retry;
static struct mesh_event_accept_completed
    mesh_event_accept_completed[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS];
static uint8_t mesh_event_accept_completed_cursor;
static struct app_mesh_event_retry_state mesh_event_accept_rx_cache;
static K_MUTEX_DEFINE(mesh_event_control_retry_scratch_lock);
static struct mesh_outbound mesh_event_control_retry_scratch;
static uint32_t mesh_direct_gateway_bulk_suppressed_until_ms;
static uint64_t mesh_gateway_route_preempt_peer_id;
static uint32_t mesh_gateway_route_preempt_deadline_ms;
static struct c5_contact_context mesh_c5_contact;
static enum ch9_event_state mesh_ch9_event_state;
static uint64_t mesh_ch9_event_peer_id;
static uint32_t mesh_ch9_event_start_ms;
static uint32_t mesh_ch9_event_end_ms;
static struct mesh_route_embedded_rx_state mesh_route_embedded_rx;
static uint64_t mesh_route_embedded_reply_peer_id;
static uint32_t mesh_route_embedded_reply_hold_until_ms;
static struct {
    bool valid;
    struct mesh_outbound outbound;
    uint8_t purpose;
    const char *reason;
    bool response_priority;
    uint8_t retry_count;
    struct app_mesh_rf_retry_state rf_retry;
    uint32_t queued_at_ms;
} mesh_c5_flood_deferred;

#define MESH_C5_DEFERRED_MAX_RETRIES 8u

struct mesh_c5_flood_tx_context {
    bool *rf_started_out;
    bool response_priority;
};

void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
static int mesh_submit_work(struct k_work *work);
static int mesh_gateway_control_send_flood(
    void *ctx,
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    struct app_mesh_flood_result *result);
static void mesh_gateway_control_priority_observed(void *ctx, int result);
static void mesh_try_route_waiting_tx(void);
static int mesh_start_tracked_tx_with_retry(const struct mesh_outbound *out,
                                            const char *reason,
                                            uint32_t *wait_retry_delay_ms,
                                            bool store_route_wait,
                                            bool *send_attempted,
                                            bool *rf_sent,
                                            bool *policy_deferred);
static int mesh_schedule_tx_timeout(void);
static void mesh_route_discovery_work_handler(struct k_work *work);
static void mesh_event_negotiation_retry_work_handler(struct k_work *work);
static void mesh_schedule_async_route_request(uint64_t target_id, const char *reason);
static bool mesh_defer_active_collection_result(const char *reason);
static bool mesh_channel9_next_required_activity(
    const struct mesh_relay_event_timing_entry *entry,
    struct mesh_event_timing *timing);
static uint32_t mesh_channel9_prepare_start_ms(const struct mesh_event_timing *timing);
static int mesh_schedule_uwb_rx(uint32_t delay_ms);
#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_preempt_save_outbox(void *ctx);
static int mesh_preempt_clear_outbox(void *ctx);
static int mesh_preempt_stage_click_handoff(void *ctx,
                                            const struct mesh_outbound *outbound);
static int mesh_preempt_commit_click_handoff(void *ctx,
                                             const struct mesh_outbound *outbound);
static int mesh_preempt_rollback_click_handoff(void *ctx,
                                               const struct mesh_outbound *outbound);
static int mesh_preempt_cancel_timeout(void *ctx);
static int mesh_preempt_schedule_timeout(void *ctx);
static int mesh_preempt_requeue_click_report(void *ctx,
                                              const struct mesh_outbound *outbound);
static int mesh_preempt_discard_requeued_click_report(
    void *ctx,
    const struct mesh_outbound *outbound);
static int mesh_preempt_cancel_active_tx(void *ctx);
static void mesh_schedule_persistence_retry(const char *reason);
#endif
static int mesh_handoff_save_child_custody(void *ctx);
static void mesh_handoff_note_result_bundle_forwarded(const struct mesh_outbound *out,
                                                      void *ctx);
static int mesh_handoff_send_result_grant(const struct mesh_outbound *out,
                                          void *ctx);
static void mesh_handoff_note_tx_sent(const struct mesh_outbound *out,
                                      void *ctx);
static int mesh_send_route_wake_train(uint64_t target_id,
                                      const struct mesh_outbound *embedded_route_req,
                                      bool *embedded_sent,
                                      uint8_t purpose,
                                      const char *reason);
static bool mesh_frame_requires_anchor_click_handoff(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim);
static bool mesh_handoff_anchor_click_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t quality,
    uint32_t observed_packet_ms);
static int mesh_send_c5_flood_now(const struct mesh_outbound *out,
                                  uint8_t purpose,
                                  const char *reason,
                                  bool send_wake_train,
                                  bool response_priority,
                                  bool single_opportunity,
                                  const struct app_mesh_command_orchestrator *command_orchestrator,
                                  struct app_mesh_flood_result *result,
                                  bool *rf_started_out);
static void mesh_c5_flood_work_handler(struct k_work *work);
struct mesh_route_capture_identity {
    uint32_t session_id;
    uint32_t flood_epoch_id;
    uint16_t reply_nonce;
};
static int mesh_listen_for_route_reply(uint64_t target_id,
                                       const char *reason,
                                       uint32_t window_ms,
                                       const struct mesh_route_capture_identity *identity,
                                       bool *route_reply_captured);
static void mesh_route_reply_handoff_after_capture(uint64_t target_id,
                                                   const char *reason);
static bool mesh_packet_is_event_control_type(uint8_t msg_type);
static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t received_at_ms);
static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t link_quality,
                                            bool *embedded_route_frame_out,
                                            bool *click_priority_out);
static bool mesh_gateway_route_test_slots_full_for(uint64_t peer_id);
static bool mesh_gateway_route_test_should_reject_route_request(
    const struct mesh_rx_pending *pending);
static void mesh_restore_anchor_low_duty_if_no_ch9(const char *reason);
static uint8_t mesh_expire_channel9_timings(uint32_t now_ms, const char *reason);
static bool mesh_find_active_channel9_timing(uint64_t peer_id,
                                             uint32_t now_ms,
                                             struct mesh_event_timing *timing);
static uint8_t mesh_advance_channel9_timing_past(uint64_t peer_id,
                                                 uint32_t now_ms,
                                                 const char *reason);
static uint8_t mesh_advance_all_channel9_timings_past(uint32_t now_ms,
                                                      const char *reason);
static void mesh_close_channel9_connection(uint64_t peer_id, const char *reason);
static int mesh_send_pending_ch9_ack_batch(const struct mesh_event_plan *plan,
                                           uint64_t peer_id,
                                           const char *reason);
#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_ch9_tx_fits_configured_slot(const struct mesh_outbound *out,
                                             const struct mesh_event_plan *plan,
                                             uint32_t now_ms,
                                             uint32_t send_start_ms,
                                             uint32_t *required_ms);
#endif
static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out);
static uint32_t mesh_ch9_slot_send_start_ms(const struct mesh_outbound *out,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms);
static bool mesh_ch9_tx_fits_plan(const struct mesh_outbound *out,
                                  const struct mesh_event_plan *plan,
                                  uint32_t now_ms,
                                  uint32_t *required_ms);
#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_ch9_slot_tx_begin(struct mesh_ch9_slot_tx_context *ctx);
static void mesh_ch9_slot_tx_end(struct mesh_ch9_slot_tx_context *ctx);
#endif
static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *out,
                                                       const char *reason,
                                                       size_t *frame_len_out);
static void mesh_wait_until_ms(uint32_t target_ms);
static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms);
static uint32_t mesh_route_test_first_event_time_ms(uint32_t now_ms);
static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms);
static int mesh_cancel_delayable(struct k_work_delayable *work);
static int mesh_submit_work(struct k_work *work);
static bool mesh_transport_paused(void);
static int mesh_transport_radio_start(const char *owner);
static void mesh_uwb_rx_rearm_work_handler(struct k_work *work);
static void mesh_persistence_retry_work_handler(struct k_work *work);
static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
                                     const struct mesh_event_plan *current_channel9_plan,
                                     uint64_t current_channel9_peer_id,
                                     bool *valid_mesh_frame,
                                     uint64_t *previous_hop_id);
static bool mesh_queue_from_frame_at_internal(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t link_quality,
    uint8_t radio_channel,
    uint32_t received_at_ms,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t current_channel9_peer_id,
    bool submit_work,
    bool *valid_mesh_frame,
    uint64_t *previous_hop_id);

static bool mesh_report_anchor_survey_discovery_is_pending(void)
{
    return mesh_report_callbacks != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending != NULL &&
           mesh_report_callbacks->anchor_survey_discovery_is_pending();
}

static uint8_t mesh_channel9_connection_count(void)
{
    uint8_t count = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (mesh_runtime.event_timings[i].valid) {
            count++;
        }
    }
    return count;
}

static bool mesh_gateway_route_test_role(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY;
}

static bool mesh_gateway_route_test_peer_active(uint64_t peer_id)
{
    if (!mesh_gateway_route_test_role() || !mesh_id_is_unicast(peer_id)) {
        return false;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (entry->valid && entry->next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static bool mesh_gateway_route_test_slots_full_for(uint64_t peer_id)
{
    return mesh_gateway_route_test_role() &&
           !mesh_gateway_route_test_peer_active(peer_id) &&
           mesh_channel9_connection_count() >= MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS;
}

static bool mesh_gateway_route_test_ch5_scan_has_capacity(void)
{
    return !mesh_gateway_route_test_role() ||
           mesh_channel9_connection_count() < MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS;
}

static void mesh_gateway_route_test_clear_preempt(uint64_t peer_id, const char *reason)
{
    if (!mesh_gateway_route_test_role() ||
        mesh_gateway_route_preempt_peer_id == 0u ||
        (mesh_id_is_unicast(peer_id) && mesh_gateway_route_preempt_peer_id != peer_id)) {
        return;
    }

    status_debug_printf("DBG_GATEWAY_CH5_PREEMPT_CLEAR peer=0x%llx reason=%s\n",
                        (unsigned long long)mesh_gateway_route_preempt_peer_id,
                        reason == NULL ? "clear" : reason);
    mesh_gateway_route_preempt_peer_id = 0u;
    mesh_gateway_route_preempt_deadline_ms = 0u;
    app_node_comm_gateway_route_refresh_response_priority_clear();
}

static bool mesh_gateway_route_test_preempt_active(uint32_t now_ms)
{
    if (!mesh_gateway_route_test_role() || mesh_gateway_route_preempt_peer_id == 0u) {
        return false;
    }
    if (uptime_deadline_reached(now_ms, mesh_gateway_route_preempt_deadline_ms)) {
        mesh_gateway_route_test_clear_preempt(0u, "timeout");
        status_debug_note("DBG_GATEWAY_CH5_PREEMPT_TIMEOUT\n");
        return false;
    }
    return true;
}

static uint32_t mesh_gateway_route_test_preempt_window_ms(uint32_t now_ms)
{
    uint32_t remaining_ms;

    if (!mesh_gateway_route_test_preempt_active(now_ms)) {
        return 0u;
    }

    remaining_ms = uptime_ms_until_deadline(now_ms,
                                            mesh_gateway_route_preempt_deadline_ms);
    if (remaining_ms == 0u) {
        return 1u;
    }
    return remaining_ms;
}

static const char *mesh_c5_contact_purpose_name(uint8_t purpose)
{
    switch (purpose) {
    case C5_CONTACT_PURPOSE_ROUTE_SOLICIT:
        return "route_solicit";
    case C5_CONTACT_PURPOSE_ROUTE_REPLY:
        return "route_reply";
    case C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH:
        return "route_contact_refresh";
    case C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD:
        return "gateway_command_flood";
    case C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD:
        return "collection_eack_flood";
    case C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT:
        return "result_offer_grant";
    case C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION:
        return "channel9_timing_negotiation";
    default:
        return "unknown";
    }
}

static void mesh_c5_contact_log(const char *phase, const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    status_debug_printf("DBG_C5_CONTACT phase=%s state=%u peer=0x%016llx purpose=%s reason=%s\n",
                        phase == NULL ? "state" : phase,
                        (unsigned int)mesh_c5_contact.state,
                        (unsigned long long)mesh_c5_contact.peer_id,
                        mesh_c5_contact_purpose_name(mesh_c5_contact.purpose),
                        reason == NULL ? "contact" : reason);
}

static void mesh_c5_contact_clear(const char *reason)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE) {
        return;
    }

    mesh_c5_contact.state = C5_CONTACT_CLOSING;
    mesh_c5_contact_log("closing", reason);
    memset(&mesh_c5_contact, 0, sizeof(mesh_c5_contact));
}

static bool mesh_c5_contact_active(uint64_t peer_id,
                                   uint8_t purpose,
                                   uint32_t now_ms)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE ||
        mesh_c5_contact.peer_id != peer_id ||
        mesh_c5_contact.purpose != purpose) {
        return false;
    }
    if (mesh_c5_contact.expires_at_ms != 0u &&
        uptime_deadline_reached(now_ms, mesh_c5_contact.expires_at_ms)) {
        mesh_c5_contact_clear("expired");
        return false;
    }
    return mesh_c5_contact.accepted;
}

static void mesh_c5_contact_open(uint64_t peer_id,
                                 uint8_t purpose,
                                 uint32_t contact_id,
                                 bool peer_was_woken,
                                 uint32_t expires_at_ms,
                                 const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return;
    }

    if (mesh_c5_contact.state != C5_CONTACT_NONE &&
        (mesh_c5_contact.peer_id != peer_id ||
         mesh_c5_contact.purpose != purpose)) {
        mesh_c5_contact_clear("replace");
    }

    mesh_c5_contact.peer_id = peer_id;
    mesh_c5_contact.contact_id = contact_id;
    mesh_c5_contact.purpose = purpose;
    mesh_c5_contact.peer_was_woken = peer_was_woken;
    mesh_c5_contact.accepted = false;
    mesh_c5_contact.opened_at_ms = now_ms;
    mesh_c5_contact.last_frame_at_ms = now_ms;
    mesh_c5_contact.expires_at_ms = expires_at_ms;
    mesh_c5_contact.state = peer_was_woken ?
                            C5_CONTACT_WAKE_PENDING :
                            C5_CONTACT_AWAKE_ACCEPTED;
    mesh_c5_contact_log("open", reason);
}

static void mesh_c5_contact_accept(uint64_t peer_id,
                                   uint8_t purpose,
                                   uint32_t expires_at_ms,
                                   const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    bool matching_contact;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return;
    }
    matching_contact = mesh_c5_contact.state != C5_CONTACT_NONE &&
                       mesh_c5_contact.peer_id == peer_id &&
                       mesh_c5_contact.purpose == purpose;
    if (matching_contact &&
        mesh_c5_contact.expires_at_ms != 0u &&
        uptime_deadline_reached(now_ms, mesh_c5_contact.expires_at_ms)) {
        mesh_c5_contact_clear("expired");
        matching_contact = false;
    }
    if (!matching_contact) {
        mesh_c5_contact_open(peer_id,
                             purpose,
                             now_ms,
                             false,
                             expires_at_ms,
                             reason);
    }
    mesh_c5_contact.accepted = true;
    mesh_c5_contact.last_frame_at_ms = now_ms;
    mesh_c5_contact.expires_at_ms = expires_at_ms;
    mesh_c5_contact.state = C5_CONTACT_AWAKE_ACCEPTED;
    mesh_c5_contact_log("accepted", reason);
}

static void mesh_c5_contact_exchange(uint64_t peer_id,
                                     uint8_t purpose,
                                     uint32_t expires_at_ms,
                                     const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();

    if (!mesh_c5_contact_active(peer_id, purpose, now_ms)) {
        mesh_c5_contact_accept(peer_id, purpose, expires_at_ms, reason);
    }
    if (mesh_c5_contact.peer_id == peer_id &&
        mesh_c5_contact.purpose == purpose) {
        mesh_c5_contact.accepted = true;
        mesh_c5_contact.last_frame_at_ms = now_ms;
        mesh_c5_contact.expires_at_ms = expires_at_ms;
        mesh_c5_contact.state = C5_CONTACT_EXCHANGE_ACTIVE;
        mesh_c5_contact_log("exchange", reason);
    }
}

static bool mesh_c5_contact_peer_active(uint64_t peer_id, uint32_t now_ms)
{
    if (mesh_c5_contact.state == C5_CONTACT_NONE ||
        mesh_c5_contact.peer_id != peer_id) {
        return false;
    }
    if (mesh_c5_contact.expires_at_ms != 0u &&
        uptime_deadline_reached(now_ms, mesh_c5_contact.expires_at_ms)) {
        mesh_c5_contact_clear("expired");
        return false;
    }
    return mesh_c5_contact.accepted;
}

static uint32_t mesh_c5_exchange_duration_ms(uint8_t purpose)
{
    switch (purpose) {
    case C5_CONTACT_PURPOSE_ROUTE_REPLY:
        return MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS;
    case C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD:
    case C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD:
        return FLOOD_WAVE_MS + FLOOD_RELAY_BURST_MS + FLOOD_POST_ROOT_GUARD_MS;
    case C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT:
    case C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION:
    case C5_CONTACT_PURPOSE_ROUTE_SOLICIT:
    case C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH:
    default:
        return MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
}

static uint32_t mesh_route_reply_listen_window_ms(uint8_t route_ttl)
{
    const struct app_mesh_c5_route_reply_window_timing timing = {
        .base_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
        .wake_train_ms = WAKE_ADV_MS,
        .post_wake_route_rx_ms = MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS,
        .wake_to_route_delay_ms = MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
        .request_flood_burst_ms = FLOOD_RELAY_BURST_MS,
        .flood_forward_wave_ms = FLOOD_WAVE_MS,
        .route_reply_exchange_ms = MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS,
        .direct_gateway_probe_ms = MESH_GATEWAY_DIRECT_PROBE_ROUND_MS,
        .guard_ms = MESH_ROUTE_TEST_REPLY_WINDOW_GUARD_MS,
    };

    return app_mesh_c5_route_reply_listen_window_ms(route_ttl, &timing);
}

static bool mesh_direct_gateway_bulk_probe_install_suppressed(uint64_t target_id,
                                                              uint32_t now_ms)
{
    if (target_id != GATEWAY_ID ||
        mesh_direct_gateway_bulk_suppressed_until_ms == 0u) {
        return false;
    }
    if (uptime_deadline_reached(now_ms,
                                mesh_direct_gateway_bulk_suppressed_until_ms)) {
        mesh_direct_gateway_bulk_suppressed_until_ms = 0u;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS_EXPIRED now=%u\n",
                                now_ms);
        }
        return false;
    }
    return true;
}

static void mesh_direct_gateway_bulk_probe_suppress(uint32_t now_ms,
                                                    const char *reason)
{
    uint32_t until_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY) {
        return;
    }

    until_ms = now_ms + ROUTE_PARENT_HOLDDOWN_MS;
    if (until_ms == 0u) {
        until_ms = 1u;
    }
    mesh_direct_gateway_bulk_suppressed_until_ms = until_ms;
    status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS now=%u until=%u reason=%s\n",
                        now_ms,
                        until_ms,
                        reason == NULL ? "direct-bulk-ack-timeout" : reason);
}

static void mesh_direct_gateway_bulk_probe_clear(const char *reason)
{
    if (mesh_direct_gateway_bulk_suppressed_until_ms == 0u) {
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_BULK_SUPPRESS_CLEAR until=%u reason=%s\n",
                            mesh_direct_gateway_bulk_suppressed_until_ms,
                            reason == NULL ? "direct-bulk-ack" : reason);
    }
    mesh_direct_gateway_bulk_suppressed_until_ms = 0u;
}

static uint32_t mesh_c5_exchange_expires_at(uint8_t purpose)
{
    return k_uptime_get_32() + mesh_c5_exchange_duration_ms(purpose);
}

static bool mesh_click_priority_active(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR && anchor_click_window_active();
}

static void mesh_coordinator_decide_now(
    const char *owner,
    struct app_mesh_coordinator_decision *decision)
{
    const uint32_t rx_queue_used = k_msgq_num_used_get(&mesh_rx_msgq);
    const uint32_t report_queue_used = report_tx_queue_used();
    const uint32_t channel9_connection_count = mesh_channel9_connection_count();
    const bool relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    const struct app_mesh_coordinator_runtime_capture capture = {
        .click_active = mesh_click_priority_active(),
        .survey_pending = DEVICE_ROLE == ROLE_ANCHOR &&
                          mesh_report_anchor_survey_discovery_is_pending(),
        .rx_queue_used = rx_queue_used,
        .report_queue_used = report_queue_used,
        .relay_tx_active = relay_tx_active,
        .route_waiting_tx_active = mesh_route_waiting_tx_valid,
        .ch9_ack_wait_active = mesh_ch9_tx_pending.active ||
            app_mesh_ch9_core_ack_wait_active(&mesh_runtime.pending,
                                              relay_tx_active),
        .ch9_ack_send_pending =
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table),
        .gateway_continuous_ch9 = mesh_gateway_route_test_role(),
    };
    bool state_changed;
    int ret;

    ret = app_mesh_command_orchestrator_decide(
        app_node_comm_gateway_control_context(),
        &capture,
        decision,
        &state_changed);
    if (ret < 0 || decision == NULL) {
        LOG_ERR("mesh coordinator capture rejected: %d", ret);
        return;
    }

    if (state_changed) {
        const char *state_name =
            app_mesh_coordinator_state_name(decision->state);

        high_debug_log_event("MESH_COORDINATOR",
                             "owner=%s state=%s reason=%s q_rx=%u q_tx=%u relay=%u route_wait=%u ack_wait=%u ack_send=%u ch9=%u",
                             owner == NULL ? "mesh" : owner,
                             state_name,
                             decision->reason == NULL ? "none" : decision->reason,
                             capture.rx_queue_used,
                             capture.report_queue_used,
                             capture.relay_tx_active ? 1u : 0u,
                             capture.route_waiting_tx_active ? 1u : 0u,
                             capture.ch9_ack_wait_active ? 1u : 0u,
                             capture.ch9_ack_send_pending ? 1u : 0u,
                             channel9_connection_count);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_COORD state=%s owner=%s reason=%s rx=%u tx=%u relay=%u route=%u ack=%u qack=%u\n",
                                state_name,
                                owner == NULL ? "mesh" : owner,
                                decision->reason == NULL ? "none" :
                                decision->reason,
                                capture.rx_queue_used,
                                capture.report_queue_used,
                                capture.relay_tx_active ? 1u : 0u,
                                capture.route_waiting_tx_active ? 1u : 0u,
                                capture.ch9_ack_wait_active ? 1u : 0u,
                                capture.ch9_ack_send_pending ? 1u : 0u);
        }
    }
}

static bool mesh_coordinator_mesh_work_allowed(const char *owner)
{
    struct app_mesh_coordinator_decision decision;

    mesh_coordinator_decide_now(owner, &decision);
    if (decision.mesh_work_allowed) {
        return true;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_COORD_MESH_DEFER owner=%s state=%s reason=%s\n",
                            owner == NULL ? "mesh" : owner,
                            app_mesh_coordinator_state_name(decision.state),
                            decision.reason == NULL ? "none" :
                            decision.reason);
    }
    return false;
}

static bool mesh_outbound_reports_local_delivery_loss(const struct mesh_outbound *out)
{
    if (out == NULL || out->packet.src_id != DEVICE_ID ||
        out->packet.dst_id == MESH_BROADCAST_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static void mesh_attach_paused_delivery_loss(struct mesh_outbound *out,
                                             const char *reason)
{
    struct app_mesh_paused_delivery_attach_result result;
    int ret;

    if (!mesh_outbound_reports_local_delivery_loss(out) ||
        app_mesh_paused_delivery_lost_count(&mesh_paused_delivery) == 0u) {
        return;
    }

    ret = app_mesh_paused_delivery_attach_loss(&mesh_paused_delivery, out, &result);
    if (ret == PROTO_OK) {
        high_debug_log_event("MESH_PAUSED_DELIVERY",
                             "phase=loss-tlv msg=0x%02x dst=0x%016llx seq=%u lost=%u reason=%s",
                             out->packet.msg_type,
                             (unsigned long long)out->packet.dst_id,
                             out->packet.seq,
                             result.lost_count,
                             reason == NULL ? "mesh" : reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_LOSS_TLV msg=0x%02x seq=%u lost=%u attach=%u update=%u reason=%s\n",
                                out->packet.msg_type,
                                out->packet.seq,
                                result.lost_count,
                                result.tlv_attached ? 1u : 0u,
                                result.tlv_updated ? 1u : 0u,
                                reason == NULL ? "mesh" : reason);
        }
        return;
    }

    LOG_WRN("mesh paused-delivery loss TLV not attached: msg=0x%02x seq=%u lost=%u ret=%d reason=%s",
            out->packet.msg_type,
            out->packet.seq,
            result.lost_count,
            ret,
            reason == NULL ? "mesh" : reason);
}

static uint8_t mesh_c5_purpose_for_packet(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return 0u;
    }

    switch (packet->msg_type) {
    case MSG_ROUTE_REQ:
        return C5_CONTACT_PURPOSE_ROUTE_SOLICIT;
    case MSG_ROUTE_REPLY:
    case MSG_ROUTE_REPLY_ACK:
        return C5_CONTACT_PURPOSE_ROUTE_REPLY;
    case MSG_GATEWAY_ROUTE_ADV:
    case MSG_GATEWAY_ACK:
    case MSG_RELAY_BUSY:
        return C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH;
    case MSG_COMMAND:
    case MSG_SURVEY_DISCOVERY_START:
        return C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD;
    case MSG_GATEWAY_COLLECTION_EACK:
        return C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD;
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_OFFER:
    case MSG_RESULT_GRANT:
    case MSG_RESULT_BUSY:
        return C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT;
    case MSG_MESH_EVENT_PROPOSE:
    case MSG_MESH_EVENT_ACCEPT:
    case MSG_MESH_EVENT_UPDATE:
    case MSG_MESH_EVENT_END:
        return C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION;
    default:
        return 0u;
    }
}

static void mesh_note_c5_control_rx(const struct proto_packet *packet,
                                    uint64_t previous_hop_id,
                                    uint8_t radio_channel,
                                    const char *reason)
{
    uint8_t purpose;

    if (radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        !mesh_id_is_unicast(previous_hop_id) ||
        previous_hop_id == DEVICE_ID) {
        return;
    }

    purpose = mesh_c5_purpose_for_packet(packet);
    if (purpose == 0u) {
        return;
    }

    mesh_c5_contact_accept(previous_hop_id,
                           purpose,
                           mesh_c5_exchange_expires_at(purpose),
                           reason == NULL ? "c5-control-rx" : reason);
}

static const char *mesh_ch9_event_state_name(enum ch9_event_state state)
{
    switch (state) {
    case CH9_EVENT_NONE:
        return "none";
    case CH9_EVENT_GRANTED:
        return "granted";
    case CH9_EVENT_TX_PAYLOAD:
        return "tx_payload";
    case CH9_EVENT_WAIT_CUSTODY_ACK:
        return "wait_custody_ack";
    case CH9_EVENT_COMPLETE:
        return "complete";
    case CH9_EVENT_BUSY_RETRY_LATER:
        return "busy_retry_later";
    case CH9_EVENT_WINDOW_EXPIRED:
        return "window_expired";
    case CH9_EVENT_PREEMPTED_BY_C5:
        return "preempted_by_c5";
    default:
        return "unknown";
    }
}

static void mesh_ch9_event_set(enum ch9_event_state state,
                               uint64_t peer_id,
                               const struct mesh_event_plan *plan,
                               const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_ch9_event_state = state;
    mesh_ch9_event_peer_id = peer_id;
    mesh_ch9_event_start_ms = plan == NULL ? 0u : plan->start_ms;
    mesh_ch9_event_end_ms = plan == NULL ? 0u : plan->end_ms;
    status_debug_printf("DBG_CH9_EVENT_STATE state=%s peer=0x%016llx start=%u end=%u reason=%s\n",
                        mesh_ch9_event_state_name(state),
                        (unsigned long long)mesh_ch9_event_peer_id,
                        mesh_ch9_event_start_ms,
                        mesh_ch9_event_end_ms,
                        reason == NULL ? "ch9-event" : reason);
}

static void mesh_ch9_event_note_persistent_ack_wait(uint64_t peer_id,
                                                    uint32_t deadline_ms,
                                                    const char *reason)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    status_debug_printf("DBG_CH9_DELIVERY_WAIT peer=0x%016llx deadline=%u reason=%s\n",
                        (unsigned long long)peer_id,
                        deadline_ms,
                        reason == NULL ? "gateway-ack" : reason);
}

static void mesh_gateway_route_test_note_channel5_contact(uint64_t peer_id,
                                                          const char *reason)
{
    if (!mesh_gateway_route_test_role() || !mesh_id_is_unicast(peer_id)) {
        return;
    }
    if (mesh_gateway_route_test_slots_full_for(peer_id)) {
        status_debug_note("DBG_GATEWAY_CH5_FULL_IGNORE\n");
        status_debug_printf("DBG_GATEWAY_CH5_FULL peer=0x%llx active=%u reason=%s\n",
                            (unsigned long long)peer_id,
                            mesh_channel9_connection_count(),
                            reason == NULL ? "ch5" : reason);
        return;
    }

    mesh_gateway_route_preempt_peer_id = peer_id;
    mesh_gateway_route_preempt_deadline_ms =
        k_uptime_get_32() + MESH_GATEWAY_ROUTE_PREEMPT_MS;
    status_debug_printf("DBG_GATEWAY_CH5_PREEMPT peer=0x%llx until=%u reason=%s\n",
                        (unsigned long long)peer_id,
                        mesh_gateway_route_preempt_deadline_ms,
                        reason == NULL ? "ch5" : reason);
}

static bool mesh_gateway_route_test_should_reject_route_request(
    const struct mesh_rx_pending *pending)
{
    if (!mesh_gateway_route_test_role() ||
        pending == NULL ||
        pending->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        pending->packet.msg_type != MSG_ROUTE_REQ) {
        return false;
    }
    if (!mesh_gateway_route_test_slots_full_for(pending->previous_hop_id)) {
        mesh_gateway_route_test_note_channel5_contact(pending->previous_hop_id,
                                                      "route-req");
        return false;
    }

    status_debug_note("DBG_GATEWAY_ROUTE_REQ_REJECT_FULL\n");
    status_debug_printf("DBG_GATEWAY_ROUTE_REQ_REJECT prev=0x%llx src=0x%llx active=%u\n",
                        (unsigned long long)pending->previous_hop_id,
                        (unsigned long long)pending->packet.src_id,
                        mesh_channel9_connection_count());
    mesh_gateway_route_test_clear_preempt(pending->previous_hop_id,
                                          "route-req-full");
    return true;
}

static void mesh_restore_anchor_low_duty_if_no_ch9(const char *reason)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        mesh_channel9_connection_count() != 0u) {
        return;
    }

    if (anchor_uwb_scan_interval_ms != CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS) {
        anchor_uwb_scan_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS;
        status_debug_note("DBG_CH9_IDLE_CH5_LOW_DUTY\n");
        high_debug_log_event("MESH_CH9_IDLE",
                             "reason=%s ch5_scan_interval_ms=%u",
                             reason == NULL ? "idle" : reason,
                             anchor_uwb_scan_interval_ms);
        LOG_INF("mesh channel-9 idle; restored low-duty channel-5 scan: interval_ms=%u reason=%s",
                anchor_uwb_scan_interval_ms,
                reason == NULL ? "idle" : reason);
    }
#else
    ARG_UNUSED(reason);
#endif
}

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
static const char *mesh_ch9_direction_name(enum mesh_relay_channel9_direction direction)
{
    switch (direction) {
    case MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM:
        return "upstream";
    case MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM:
        return "downstream";
    case MESH_RELAY_CHANNEL9_DIRECTION_AMBIGUOUS:
        return "ambiguous";
    case MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *mesh_ch9_guard_reason_name(enum mesh_relay_channel9_guard_reason reason)
{
    switch (reason) {
    case MESH_RELAY_CHANNEL9_GUARD_OK:
        return "ok";
    case MESH_RELAY_CHANNEL9_GUARD_REPLACED_PEER:
        return "replaced-peer";
    case MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_NEW_PEER:
        return "ambiguous-new-peer";
    case MESH_RELAY_CHANNEL9_GUARD_AMBIGUOUS_ACTIVE_PEER:
        return "ambiguous-active-peer";
    case MESH_RELAY_CHANNEL9_GUARD_TOO_MANY_PEERS:
        return "too-many-peers";
    case MESH_RELAY_CHANNEL9_GUARD_DIRECTION_BUSY:
        return "direction-busy";
    default:
        return "unknown";
    }
}
#endif

static int mesh_install_channel9_timing_direction(
    uint64_t peer_id,
    const struct mesh_event_timing *timing,
    enum mesh_relay_channel9_direction direction,
    const char *reason)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        if (DEVICE_ROLE == ROLE_GATEWAY) {
            if (!mesh_event_timing_local_rx_slot(timing)) {
                status_debug_note("DBG_GATEWAY_CH9_REJECT_TX_FIRST\n");
                status_debug_printf("DBG_GATEWAY_CH9_REJECT peer=0x%llx active=%u reason=tx-first\n",
                                    (unsigned long long)peer_id,
                                    mesh_channel9_connection_count());
                LOG_WRN("mesh gateway rejected channel-9 timing that does not start RX: peer=0x%016llx reason=%s",
                        (unsigned long long)peer_id,
                        reason == NULL ? "install" : reason);
                return PROTO_ERR_MALFORMED;
            }
            status_debug_note("DBG_GATEWAY_CH9_CONTINUOUS_RX_NO_SLOT\n");
            status_debug_printf("DBG_GATEWAY_CH9_NO_SLOT peer=0x%llx cnt=%u next=%u reason=%s\n",
                                (unsigned long long)peer_id,
                                timing->event_counter,
                                timing->next_event_time_ms,
                                reason == NULL ? "install" : reason);
            return PROTO_OK;
        }
        struct mesh_relay_channel9_guard_status guard = {0};
        int ret;

        if (direction == MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM ||
            direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
            ret = mesh_relay_set_channel9_timing_guarded_direction(
                &mesh_runtime,
                peer_id,
                timing,
                direction,
                MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                &guard);
        } else {
            ret = mesh_relay_set_channel9_timing_guarded(
                &mesh_runtime,
                peer_id,
                timing,
                MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                &guard);
        }

        if (ret != PROTO_OK) {
            const char *guard_reason = mesh_ch9_guard_reason_name(guard.reason);
            const char *direction = mesh_ch9_direction_name(guard.direction);
            const char *conflict_direction =
                mesh_ch9_direction_name(guard.conflict_direction);

            status_debug_note("DBG_CH9_GUARD_REJECT\n");
            status_debug_printf("DBG_CH9_GUARD peer=0x%llx ret=%d reason=%u dir=%u active=%u conflict=0x%llx cdir=%u\n",
                                (unsigned long long)peer_id,
                                ret,
                                (unsigned int)guard.reason,
                                (unsigned int)guard.direction,
                                guard.active_peer_count,
                                (unsigned long long)guard.conflict_peer_id,
                                (unsigned int)guard.conflict_direction);
            high_debug_log_event("MESH_CH9_GUARD",
                                 "phase=reject install_reason=%s guard_reason=%s peer=0x%016llx direction=%s active=%u conflict=0x%016llx conflict_direction=%s ret=%d",
                                 reason == NULL ? "install" : reason,
                                 guard_reason,
                                 (unsigned long long)peer_id,
                                 direction,
                                 guard.active_peer_count,
                                 (unsigned long long)guard.conflict_peer_id,
                                 conflict_direction,
                                 ret);
            LOG_WRN("mesh route-test channel-9 timing rejected: peer=0x%016llx reason=%s direction=%s active=%u conflict=0x%016llx conflict_direction=%s ret=%d",
                    (unsigned long long)peer_id,
                    guard_reason,
                    direction,
                    guard.active_peer_count,
                    (unsigned long long)guard.conflict_peer_id,
                    conflict_direction,
                    ret);
        }
        return ret;
    }
#else
    ARG_UNUSED(direction);
    ARG_UNUSED(reason);
#endif

    return mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, timing);
}

static int mesh_install_channel9_timing(uint64_t peer_id,
                                        const struct mesh_event_timing *timing,
                                        const char *reason)
{
    return mesh_install_channel9_timing_direction(
        peer_id,
        timing,
        MESH_RELAY_CHANNEL9_DIRECTION_UNKNOWN,
        reason);
}

static bool mesh_ch9_ack_batch_clear_for_peer(uint64_t peer_id,
                                              const char *reason)
{
    const struct app_mesh_ch9_ack_batch *batch;
    uint8_t count;
    uint16_t first_seq;

    if (!mesh_id_is_unicast(peer_id)) {
        return false;
    }
    batch = app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    if (batch == NULL) {
        return false;
    }

    count = batch->count;
    first_seq = batch->preserve_payload ? batch->template_ack.packet.seq :
                count == 0u ? 0u : batch->entries[0].seq;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_ACK_CLEAR peer=0x%llx n=%u first=%u reason=%s\n",
                            (unsigned long long)peer_id,
                            count,
                            first_seq,
                            reason == NULL ? "clear" : reason);
    }
    return app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer_id);
}

static uint8_t mesh_ch9_ack_batch_count_for_peer(uint64_t peer_id)
{
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);

    return batch == NULL ? 0u : batch->count;
}

static uint8_t mesh_expire_channel9_timings(uint32_t now_ms, const char *reason)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
            const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

            if (!entry->valid || mesh_event_timing_usable(&entry->timing, now_ms)) {
                continue;
            }
            status_debug_note("DBG_CH9_TIMING_EXPIRE_DETAIL\n");
            high_debug_log_event("MESH_CH9_TIMING",
                                 "phase=expire-detail reason=%s index=%u peer=0x%016llx now_ms=%u next_ms=%u last_peer_ms=%u supervision_ms=%u event_counter=%u local_tx_even=%u route_fresh=%u timing_fresh=%u fallback=%u",
                                 reason == NULL ? "expire" : reason,
                                 i,
                                 (unsigned long long)entry->next_hop_id,
                                 now_ms,
                                 entry->timing.next_event_time_ms,
                                 entry->timing.last_successful_ch9_event_ms,
                                 entry->timing.supervision_timeout_ms,
                                 entry->timing.event_counter,
                                 entry->timing.local_tx_on_even_events ? 1u : 0u,
                                 entry->timing.route_fresh ? 1u : 0u,
                                 entry->timing.timing_fresh ? 1u : 0u,
                                 entry->timing.fallback_required ? 1u : 0u);
            (void)mesh_ch9_ack_batch_clear_for_peer(entry->next_hop_id,
                                                    reason == NULL ? "timing-expire" : reason);
        }
    }

    uint8_t expired = mesh_relay_expire_channel9_timings(&mesh_runtime, now_ms);

    if (expired > 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TIMING_EXPIRED\n");
        }
        high_debug_log_event("MESH_CH9_TIMING",
                             "phase=expired count=%u reason=%s now_ms=%u active=%u",
                             expired,
                             reason == NULL ? "expire" : reason,
                             now_ms,
                             mesh_channel9_connection_count());
        mesh_restore_anchor_low_duty_if_no_ch9(reason);
    }
    return expired;
}

static bool mesh_find_active_channel9_timing(uint64_t peer_id,
                                             uint32_t now_ms,
                                             struct mesh_event_timing *timing)
{
    if (!mesh_id_is_unicast(peer_id) || timing == NULL) {
        return false;
    }

    (void)mesh_expire_channel9_timings(now_ms, "find-active");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (!entry->valid ||
            entry->next_hop_id != peer_id ||
            !mesh_event_timing_usable(&entry->timing, now_ms)) {
            continue;
        }
        *timing = entry->timing;
        return true;
    }

    return false;
}

static uint32_t mesh_channel9_skip_reference_ms(uint32_t now_ms)
{
    if (MESH_EVENT_RX_LATE_GUARD_MS > 0u && now_ms > MESH_EVENT_RX_LATE_GUARD_MS) {
        return now_ms - MESH_EVENT_RX_LATE_GUARD_MS;
    }
    return now_ms;
}

static uint8_t mesh_advance_channel9_entry_past(struct mesh_relay_event_timing_entry *entry,
                                                uint32_t now_ms,
                                                const char *reason)
{
    uint8_t skipped;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || entry == NULL || !entry->valid) {
        return 0u;
    }

    skipped = mesh_event_skip_elapsed(&entry->timing,
                                      mesh_channel9_skip_reference_ms(now_ms),
                                      &mesh_event_stats);
    if (skipped > 0u) {
        status_debug_printf("DBG_CH9_SKIP_STALE n=%u now=%u next=%u cnt=%u reason=%s\n",
                            skipped,
                            now_ms,
                            entry->timing.next_event_time_ms,
                            entry->timing.event_counter,
                            reason == NULL ? "skip" : reason);
    }
    return skipped;
}

static uint8_t mesh_advance_channel9_timing_past(uint64_t peer_id,
                                                 uint32_t now_ms,
                                                 const char *reason)
{
    if (!mesh_id_is_unicast(peer_id)) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (entry->valid && entry->next_hop_id == peer_id) {
            return mesh_advance_channel9_entry_past(entry, now_ms, reason);
        }
    }

    return 0u;
}

static uint8_t mesh_advance_all_channel9_timings_past(uint32_t now_ms,
                                                      const char *reason)
{
    uint8_t skipped_total = 0u;

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        uint8_t skipped = mesh_advance_channel9_entry_past(&mesh_runtime.event_timings[i],
                                                           now_ms,
                                                           reason);

        if ((uint8_t)(UINT8_MAX - skipped_total) < skipped) {
            skipped_total = UINT8_MAX;
        } else {
            skipped_total += skipped;
        }
    }

    return skipped_total;
}

static void mesh_note_channel9_local_tx(uint64_t next_hop_id, uint32_t event_start_ms)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        mesh_relay_note_channel9_tx(&mesh_runtime, next_hop_id, event_start_ms);
    } else {
        mesh_relay_note_channel9_success(&mesh_runtime, next_hop_id, event_start_ms);
    }
}

static void mesh_report_note_anchor_uwb_awake_since(int64_t start_ms,
                                                    uint32_t already_counted_us)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_note_uwb_awake_since != NULL) {
        mesh_report_callbacks->anchor_note_uwb_awake_since(start_ms, already_counted_us);
    }
}

static void mesh_report_anchor_handle_local_command(const struct proto_packet *packet,
                                                    const uint8_t *payload,
                                                    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_local_command != NULL) {
        mesh_report_callbacks->anchor_handle_local_command(packet, payload, payload_len);
    }
}

static void mesh_report_anchor_handle_survey_discovery_start(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_discovery_start != NULL) {
        mesh_report_callbacks->anchor_handle_survey_discovery_start(packet, payload, payload_len);
    }
}

static void mesh_report_anchor_handle_survey_pair_prepare(const struct proto_packet *packet,
                                                          const uint8_t *payload,
                                                          size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_survey_pair_prepare != NULL) {
        mesh_report_callbacks->anchor_handle_survey_pair_prepare(packet, payload, payload_len);
    }
}

static int mesh_report_gateway_handle_survey_discovery_report(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint8_t radio_channel,
    uint8_t link_quality)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_handle_survey_discovery_report != NULL) {
        return mesh_report_callbacks->gateway_handle_survey_discovery_report(
            packet,
            payload,
            payload_len,
            previous_hop_id,
            radio_channel,
            link_quality);
    }

    return -ENOTSUP;
}

static bool mesh_gateway_delivery_requires_semantic_acceptance(
    const struct proto_packet *packet)
{
    if (packet == NULL || DEVICE_ROLE != ROLE_GATEWAY ||
        packet->dst_id != DEVICE_ID ||
        (packet->flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_COMMAND_RESULT:
    case MSG_RESULT_BUNDLE:
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return true;
    default:
        return false;
    }
}

static int mesh_gateway_accept_semantic_delivery(
    const struct mesh_rx_pending *pending)
{
    const struct mesh_event_plan *current_channel9_plan;

    if (pending == NULL ||
        !mesh_gateway_delivery_requires_semantic_acceptance(&pending->packet)) {
        return -EINVAL;
    }
    current_channel9_plan = pending->current_channel9_plan_valid ?
                            &pending->current_channel9_plan : NULL;

    switch (pending->packet.msg_type) {
    case MSG_COMMAND_RESULT:
        return gateway_note_command_result(&pending->packet,
                                           pending->payload,
                                           pending->payload_len,
                                           pending->previous_hop_id,
                                           pending->radio_channel,
                                           current_channel9_plan);
    case MSG_RESULT_BUNDLE:
        return gateway_note_command_result_bundle(&pending->packet,
                                                  pending->payload,
                                                  pending->payload_len,
                                                  pending->previous_hop_id,
                                                  pending->radio_channel,
                                                  current_channel9_plan);
    case MSG_SURVEY_DISCOVERY_REPORT:
    case MSG_SURVEY_PAIR_RESULT:
        return mesh_report_gateway_handle_survey_discovery_report(
            &pending->packet,
            pending->payload,
            pending->payload_len,
            pending->previous_hop_id,
            pending->radio_channel,
            pending->link_quality);
    default:
        return -EINVAL;
    }
}

static void mesh_rx_pending_refresh_age(struct mesh_rx_pending *pending, uint32_t now_ms)
{
    if (pending == NULL) {
        return;
    }

    if (pending->received_at_ms != 0u) {
        packet_age_add_elapsed(&pending->packet, now_ms - pending->received_at_ms);
    }
    pending->received_at_ms = now_ms;
}

int mesh_preempt_for_click_event(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_click_preempt_plan plan;
    struct app_mesh_click_preempt_ops ops = {
        .save_outbox = mesh_preempt_save_outbox,
        .clear_outbox = mesh_preempt_clear_outbox,
        .stage_click_handoff = mesh_preempt_stage_click_handoff,
        .commit_click_handoff = mesh_preempt_commit_click_handoff,
        .rollback_click_handoff = mesh_preempt_rollback_click_handoff,
        .cancel_timeout = mesh_preempt_cancel_timeout,
        .schedule_timeout = mesh_preempt_schedule_timeout,
        .requeue_click_report = mesh_preempt_requeue_click_report,
        .discard_requeued_click_report = mesh_preempt_discard_requeued_click_report,
        .cancel_active_tx = mesh_preempt_cancel_active_tx,
    };
    struct app_mesh_click_preempt_result result;
    int ret;

    if (mesh_prepare_click_preemption(&mesh_runtime,
                                      DEVICE_ID,
                                      k_uptime_get_32(),
                                      &plan) != PROTO_OK) {
        return -EINVAL;
    }

    ret = app_mesh_apply_click_preempt_plan(&plan, &ops, &result);

    if (result.outbox_saved) {
        LOG_INF("mesh pending collection result deferred: reason=click-preempt");
    }
    if (result.outbox_saved || result.outbox_cleared) {
        LOG_INF("anchor click discovery preempted active mesh TX");
    }
    if (result.click_report_requeue_failed) {
        LOG_ERR("click preemption retained the committed handoff because click-report queue admission failed: ret=%d",
                result.click_report_requeue_ret);
    }
    if (ret < 0) {
        LOG_ERR("click preemption custody operation failed: ret=%d save=%d stage=%d commit=%d rollback=%d schedule=%d requeue=%d cancel=%d clear=%d owner=%u",
                ret,
                result.save_outbox_ret,
                result.stage_click_handoff_ret,
                result.commit_click_handoff_ret,
                result.rollback_click_handoff_ret,
                result.schedule_timeout_ret,
                result.click_report_requeue_ret,
                result.cancel_timeout_ret,
                result.clear_outbox_ret,
                (unsigned int)result.custody_owner);
        return ret;
    }
#endif
    return 0;
}

static void mesh_schedule_persistence_retry(const char *reason)
{
    uint32_t delay_ms = discovery_assignment_retry_backoff_ms(
        mesh_persistence_retry_round,
        sys_rand32_get());

    if (mesh_persistence_retry_round < UINT8_MAX) {
        mesh_persistence_retry_round++;
    }
    (void)k_work_reschedule(&mesh_persistence_retry_work, K_MSEC(delay_ms));
    LOG_WRN("mesh persistence retry scheduled: reason=%s round=%u delay_ms=%u",
            reason == NULL ? "unknown" : reason,
            mesh_persistence_retry_round,
            delay_ms);
}

static int mesh_save_outbox_durable(const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    int ret = app_mesh_persistence_save_outbox(&mesh_runtime, now_ms);

    if (ret < 0) {
        mesh_outbox_persistence_dirty = true;
        mesh_schedule_persistence_retry(reason);
    } else {
        mesh_outbox_persistence_dirty = false;
        ret = app_mesh_persistence_complete_click_handoff(&mesh_runtime, now_ms);
        if (ret < 0 && ret != -ENOTSUP) {
            mesh_outbox_persistence_dirty = true;
            mesh_schedule_persistence_retry("click-handoff-cleanup");
            LOG_WRN("mesh committed click handoff cleanup failed: %d", ret);
            return ret;
        }
        mesh_persistence_retry_round = 0u;
    }
    return ret;
}

static int mesh_save_child_custody_durable(const char *reason)
{
    int ret = app_mesh_persistence_save_child_custody(&mesh_runtime,
                                                      k_uptime_get_32());

    if (ret < 0) {
        mesh_child_custody_persistence_dirty = true;
        mesh_schedule_persistence_retry(reason);
    } else {
        mesh_child_custody_persistence_dirty = false;
        mesh_persistence_retry_round = 0u;
    }
    return ret;
}

static void mesh_persistence_retry_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (mesh_outbox_persistence_dirty) {
        if (!mesh_relay_tx_active(&mesh_runtime)) {
            mesh_outbox_persistence_dirty = false;
        } else if (mesh_save_outbox_durable("outbox-dirty") < 0) {
            return;
        }
    }
    if (mesh_child_custody_persistence_dirty &&
        mesh_save_child_custody_durable("child-custody-dirty") < 0) {
        return;
    }
    mesh_persistence_retry_round = 0u;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_preempt_save_outbox(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_save_outbox_durable("click-preempt");
}

static int mesh_preempt_clear_outbox(void *ctx)
{
    const struct proto_packet *released_packet =
        mesh_relay_tx_active(&mesh_runtime) ?
        &mesh_runtime.pending.packet : NULL;

    ARG_UNUSED(ctx);

    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_survey_delivery_transport_released != NULL) {
        mesh_report_callbacks->anchor_survey_delivery_transport_released(
            released_packet, true);
    }
    return app_mesh_persistence_clear_outbox();
}

static int mesh_preempt_stage_click_handoff(void *ctx,
                                            const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return app_mesh_persistence_stage_click_handoff(&mesh_runtime,
                                                     k_uptime_get_32());
}

static int mesh_preempt_commit_click_handoff(void *ctx,
                                             const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return app_mesh_persistence_commit_click_handoff(&mesh_runtime,
                                                      k_uptime_get_32());
}

static int mesh_preempt_rollback_click_handoff(void *ctx,
                                               const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);
    ARG_UNUSED(outbound);

    return app_mesh_persistence_rollback_click_handoff();
}

static int mesh_preempt_cancel_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_cancel_delayable(&mesh_tx_timeout_work);
}

static int mesh_preempt_schedule_timeout(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_schedule_tx_timeout();
}

static int mesh_preempt_requeue_click_report(void *ctx,
                                              const struct mesh_outbound *outbound)
{
    ARG_UNUSED(ctx);

    return queue_anchor_report(outbound);
}

static bool mesh_preempt_outbound_matches(const struct mesh_outbound *left,
                                          const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == right->packet.msg_type &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq &&
           left->payload_len == right->payload_len &&
           (left->payload_len == 0u ||
            memcmp(left->payload, right->payload, left->payload_len) == 0);
}

static uint32_t mesh_preempt_report_queue_count(void *ctx)
{
    return k_msgq_num_used_get((struct k_msgq *)ctx);
}

static int mesh_preempt_report_queue_get(struct mesh_outbound *outbound,
                                         void *ctx)
{
    return k_msgq_get((struct k_msgq *)ctx, outbound, K_NO_WAIT);
}

static int mesh_preempt_report_queue_put(const struct mesh_outbound *outbound,
                                         void *ctx)
{
    return k_msgq_put((struct k_msgq *)ctx, outbound, K_NO_WAIT);
}

static int mesh_preempt_report_queue_recover(
    const struct mesh_outbound *outbound,
    void *ctx)
{
    ARG_UNUSED(ctx);
    if (report_tx_queue_recovery_valid) {
        return -ENOSPC;
    }
    report_tx_queue_overflow_dropped = *outbound;
    report_tx_queue_recovery_valid = true;
    report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
    return 0;
}

static bool mesh_preempt_report_queue_matches(
    const struct mesh_outbound *candidate,
    const struct mesh_outbound *target,
    void *ctx)
{
    ARG_UNUSED(ctx);
    return mesh_preempt_outbound_matches(candidate, target);
}

static int mesh_preempt_discard_requeued_click_report(
    void *ctx,
    const struct mesh_outbound *outbound)
{
    struct mesh_outbound candidate;
    const struct app_mesh_queue_remove_ops ops = {
        .count = mesh_preempt_report_queue_count,
        .get = mesh_preempt_report_queue_get,
        .put = mesh_preempt_report_queue_put,
        .recover = mesh_preempt_report_queue_recover,
        .matches = mesh_preempt_report_queue_matches,
        .ctx = &report_tx_msgq,
    };
    bool removed = false;
    int ret;

    ARG_UNUSED(ctx);
    if (outbound == NULL) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT);
    if (ret != 0) {
        return ret;
    }
    if (report_tx_queue_recovery_valid) {
        k_mutex_unlock(&report_tx_queue_overflow_lock);
        return -EBUSY;
    }
    ret = app_mesh_queue_remove_first(&ops, outbound, &candidate, &removed);
    k_mutex_unlock(&report_tx_queue_overflow_lock);
    return ret;
}

static int mesh_preempt_cancel_active_tx(void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_cancel_tx(&mesh_runtime);
    return 0;
}
#endif

static int mesh_collection_deferral_save_outbox(struct mesh_relay *relay,
                                                uint32_t now_ms,
                                                void *ctx)
{
    ARG_UNUSED(ctx);

    ARG_UNUSED(relay);
    ARG_UNUSED(now_ms);

    return mesh_save_outbox_durable("collection-deferral");
}

static int mesh_collection_deferral_schedule_retry(void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_schedule_tx_timeout();
    return 0;
}

static int mesh_handoff_save_child_custody(void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_save_child_custody_durable("handoff");
}

static void mesh_handoff_note_result_bundle_forwarded(const struct mesh_outbound *out,
                                                      void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_result_bundle_note_forwarded(&mesh_runtime, out);
}

static int mesh_handoff_send_result_grant(const struct mesh_outbound *out,
                                          void *ctx)
{
    ARG_UNUSED(ctx);

    return mesh_send_c5_control(out,
                                C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
                                MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
                                "result-grant");
}

static void mesh_handoff_note_tx_sent(const struct mesh_outbound *out,
                                      void *ctx)
{
    ARG_UNUSED(ctx);

    mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
}

static int mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_relay_tx_active(&mesh_runtime) &&
        !mesh_ch9_tx_pending.active &&
        !mesh_relay_result_bundle_pending(&mesh_runtime)) {
        return mesh_cancel_delayable(&mesh_tx_timeout_work);
    }

    deadline = UINT32_MAX;
    if (mesh_relay_tx_active(&mesh_runtime)) {
        deadline = mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_RETRY_BACKOFF ?
                   mesh_runtime.pending.retry_after_ms :
                   mesh_runtime.pending.gateway_ack_deadline_ms;
    }
    if (mesh_ch9_tx_pending.active &&
        (deadline == UINT32_MAX ||
         uptime_deadline_reached(deadline, mesh_ch9_tx_pending.deadline_ms))) {
        deadline = mesh_ch9_tx_pending.deadline_ms;
    }
    if (mesh_relay_result_bundle_pending(&mesh_runtime) &&
        (deadline == UINT32_MAX ||
         uptime_deadline_reached(deadline,
                                 mesh_relay_result_bundle_due_ms(&mesh_runtime)))) {
        deadline = mesh_relay_result_bundle_due_ms(&mesh_runtime);
    }
    delay_ms = uptime_ms_until_deadline(now, deadline);
    return mesh_reschedule_delayable(&mesh_tx_timeout_work, delay_ms);
}

static bool mesh_defer_active_collection_result(const char *reason)
{
    const uint32_t now_ms = k_uptime_get_32();
    const struct app_mesh_collection_deferral_ops ops = {
        .save_outbox = mesh_collection_deferral_save_outbox,
        .schedule_retry = mesh_collection_deferral_schedule_retry,
    };
    struct app_mesh_collection_deferral_result result;

    if (!app_mesh_collection_defer_active_result(&mesh_runtime,
                                                now_ms,
                                                sys_rand32_get(),
                                                &ops,
                                                &result)) {
        return false;
    }

    if (!result.outbox_saved) {
        LOG_WRN("mesh pending collection result deferred without persisted outbox: ret=%d reason=%s",
                result.save_ret,
                reason == NULL ? "defer" : reason);
    }
    if (!result.retry_scheduled) {
        LOG_WRN("mesh pending collection result deferred without retry schedule: ret=%d reason=%s",
                result.schedule_ret,
                reason == NULL ? "defer" : reason);
    }
    LOG_INF("mesh pending collection result deferred: reason=%s",
            reason == NULL ? "defer" : reason);
    return true;
}

static bool mesh_send_failure_retryable(int ret)
{
    return ret == -EIO || ret == -EBUSY || ret == -EAGAIN;
}

static struct app_mesh_rf_retry_key mesh_rf_retry_packet_key(
    const struct proto_packet *packet,
    enum app_mesh_rf_retry_operation operation)
{
    return (struct app_mesh_rf_retry_key) {
        .source_id = packet->src_id,
        .destination_id = packet->dst_id,
        .session_id = packet->session_id,
        .sequence = packet->seq,
        .message_type = packet->msg_type,
        .operation = (uint8_t)operation,
    };
}

static struct app_mesh_rf_retry_state *mesh_route_request_rf_retry_state(
    enum app_mesh_route_request_rf_phase phase)
{
    return phase == APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX ?
           &mesh_route_request_control_rf_retry :
           &mesh_route_request_wake_rf_retry;
}

static struct app_mesh_rf_retry_key mesh_route_request_rf_retry_key(
    const struct mesh_outbound *route_request,
    uint64_t target_id,
    enum app_mesh_route_request_rf_phase phase)
{
    struct app_mesh_rf_retry_key key = mesh_rf_retry_packet_key(
        &route_request->packet,
        phase == APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX ?
            APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_CONTROL :
            APP_MESH_RF_RETRY_OPERATION_ROUTE_REQUEST_WAKE);

    /* The discovery session survives a pre-RF restore; the packet seq does not. */
    key.destination_id = target_id;
    key.sequence = 0u;
    return key;
}

static uint32_t mesh_rf_retry_next_delay_ms(
    struct app_mesh_rf_retry_state *state,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    const char *reason)
{
    uint32_t delay_ms = app_mesh_rf_retry_next_delay_ms(
        state, key, policy, sys_rand32_get());

    if (delay_ms == 0u) {
        uint32_t random_value = sys_rand32_get();

        LOG_ERR("mesh RF retry identity rejected: reason=%s src=0x%016llx session=%u seq=%u op=%u",
                reason == NULL ? "rf-retry" : reason,
                (unsigned long long)(key == NULL ? 0u : key->source_id),
                key == NULL ? 0u : key->session_id,
                key == NULL ? 0u : key->sequence,
                key == NULL ? 0u : key->operation);
        delay_ms = policy == APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN ?
            app_wake_train_politeness_backoff_ms(0u, random_value) :
            25u + (random_value % 51u);
    }
    return delay_ms;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t mesh_rf_retry_bank_next_delay_ms(
    struct app_mesh_rf_retry_bank *bank,
    const struct app_mesh_rf_retry_key *key,
    enum app_mesh_rf_retry_policy policy,
    const char *reason)
{
    uint32_t delay_ms = app_mesh_rf_retry_bank_next_delay_ms(
        bank, key, policy, sys_rand32_get());

    if (delay_ms == 0u) {
        uint32_t random_value = sys_rand32_get();

        LOG_ERR("mesh RF retry bank rejected identity: reason=%s src=0x%016llx session=%u seq=%u op=%u",
                reason == NULL ? "rf-retry" : reason,
                (unsigned long long)(key == NULL ? 0u : key->source_id),
                key == NULL ? 0u : key->session_id,
                key == NULL ? 0u : key->sequence,
                key == NULL ? 0u : key->operation);
        delay_ms = policy == APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN ?
            app_wake_train_politeness_backoff_ms(0u, random_value) :
            25u + (random_value % 51u);
    }
    return delay_ms;
}
#endif

static const char *route_delivery_action_name(enum route_delivery_action action)
{
    switch (action) {
    case ROUTE_DELIVERY_RETRY_CURRENT:
        return "retry-current";
    case ROUTE_DELIVERY_TRY_ALTERNATE:
        return "try-alternate";
    case ROUTE_DELIVERY_DISCOVER:
        return "discover";
    default:
        return "unknown";
    }
}

static void report_tx_set_retry_delay_override(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    report_tx_retry_delay_override_ms = delay_ms;
    report_tx_retry_delay_override_valid = true;
#else
    ARG_UNUSED(delay_ms);
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool report_tx_consume_retry_delay_override(uint32_t *delay_ms)
{
    if (!report_tx_retry_delay_override_valid || delay_ms == NULL) {
        return false;
    }
    *delay_ms = report_tx_retry_delay_override_ms;
    report_tx_retry_delay_override_valid = false;
    return true;
}
#endif

static bool mesh_route_reply_handoff_applies(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE != ROLE_GATEWAY;
}

static void mesh_route_reply_handoff_begin(void)
{
    if (!mesh_route_reply_handoff_applies()) {
        return;
    }

    mesh_route_reply_handoff_pending = true;
    mesh_route_reply_handoff_deadline_ms =
        k_uptime_get_32() + MESH_ROUTE_TEST_REPLY_HANDOFF_WAIT_MS;
    status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_WAIT until=%u\n",
                        mesh_route_reply_handoff_deadline_ms);
}

static void mesh_route_reply_handoff_clear(const char *reason)
{
    if (!mesh_route_reply_handoff_applies() ||
        !mesh_route_reply_handoff_pending) {
        return;
    }

    status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_CLEAR reason=%s\n",
                        reason == NULL ? "clear" : reason);
    mesh_route_reply_handoff_pending = false;
    mesh_route_reply_handoff_deadline_ms = 0u;
}

static bool mesh_route_reply_handoff_active(void)
{
    if (!mesh_route_reply_handoff_applies() ||
        !mesh_route_reply_handoff_pending) {
        return false;
    }
    if (uptime_deadline_reached(k_uptime_get_32(),
                                mesh_route_reply_handoff_deadline_ms)) {
        status_debug_note("DBG_ROUTE_REPLY_HANDOFF_EXPIRE\n");
        mesh_route_reply_handoff_pending = false;
        mesh_route_reply_handoff_deadline_ms = 0u;
        return false;
    }
    return true;
}

static void mesh_route_reply_handoff_after_capture(uint64_t target_id,
                                                   const char *reason)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_ROUTE_REPLY_HANDOFF\n");
        status_debug_printf("DBG_ROUTE_REPLY_HANDOFF_STATE q=%u wait=%u relay=%u ch9pend=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                            mesh_ch9_tx_pending.active ? 1u : 0u);
    }
    mesh_route_reply_handoff_begin();
    if (!mesh_ch9_tx_pending.active &&
        !mesh_relay_tx_active(&mesh_runtime)) {
        (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REPLY_CANCEL_RETRY\n");
        }
    }
    LOG_INF("mesh route reply captured; waiting for queued route processing: target=0x%016llx reason=%s",
            (unsigned long long)target_id,
            reason == NULL ? "route" : reason);
}

static int mesh_route_wake_encode_suffix(const struct mesh_outbound *route_req,
                                         uint8_t *out,
                                         size_t out_cap,
                                         size_t *written)
{
    size_t payload_len;
    size_t total_len;
    uint16_t crc;

    if (route_req == NULL || out == NULL || written == NULL) {
        return -EINVAL;
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        route_req->packet.msg_type != MSG_ROUTE_REQ ||
        route_req->packet.src_id != DEVICE_ID ||
        route_req->packet.dst_id != MESH_BROADCAST_ID ||
        route_req->next_hop_id != MESH_BROADCAST_ID ||
        route_req->payload_len != route_req->packet.payload_len) {
        return -EINVAL;
    }

    payload_len = route_req->payload_len;
    if (payload_len == 0u ||
        payload_len > MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES ||
        payload_len > UINT8_MAX) {
        return -EMSGSIZE;
    }

    total_len = MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + payload_len +
                MESH_ROUTE_WAKE_ROUTE_CRC_LEN;
    if (out_cap < total_len) {
        return -ENOSPC;
    }

    out[0] = MESH_ROUTE_WAKE_ROUTE_MAGIC0;
    out[1] = MESH_ROUTE_WAKE_ROUTE_MAGIC1;
    out[2] = MESH_ROUTE_WAKE_ROUTE_VERSION;
    out[3] = MSG_ROUTE_REQ;
    out[4] = route_req->packet.flags;
    out[5] = route_req->packet.ttl;
    proto_put_u32_le(&out[6], route_req->packet.session_id);
    proto_put_u16_le(&out[10], route_req->packet.seq);
    out[12] = (uint8_t)payload_len;
    memcpy(&out[MESH_ROUTE_WAKE_ROUTE_HEADER_LEN], route_req->payload, payload_len);

    crc = proto_crc16_ccitt_false(out, total_len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN);
    proto_put_u16_le(&out[total_len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN], crc);
    *written = total_len;
    return 0;
}

static int mesh_route_wake_decode_suffix(const uint8_t *data,
                                         size_t len,
                                         uint64_t source_id,
                                         struct proto_packet *packet,
                                         uint8_t *payload,
                                         size_t payload_cap,
                                         size_t *payload_len)
{
    uint8_t declared_payload_len;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (data == NULL || packet == NULL || payload == NULL ||
        payload_len == NULL || !mesh_id_is_unicast(source_id)) {
        return -EINVAL;
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        len < MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + MESH_ROUTE_WAKE_ROUTE_CRC_LEN) {
        return -EINVAL;
    }
    if (data[0] != MESH_ROUTE_WAKE_ROUTE_MAGIC0 ||
        data[1] != MESH_ROUTE_WAKE_ROUTE_MAGIC1 ||
        data[2] != MESH_ROUTE_WAKE_ROUTE_VERSION ||
        data[3] != MSG_ROUTE_REQ) {
        return -EBADMSG;
    }

    declared_payload_len = data[12];
    if (declared_payload_len == 0u ||
        declared_payload_len > MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES ||
        declared_payload_len > payload_cap ||
        len != MESH_ROUTE_WAKE_ROUTE_HEADER_LEN + declared_payload_len +
               MESH_ROUTE_WAKE_ROUTE_CRC_LEN) {
        return -EMSGSIZE;
    }

    expected_crc = proto_get_u16_le(&data[len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN]);
    actual_crc = proto_crc16_ccitt_false(data, len - MESH_ROUTE_WAKE_ROUTE_CRC_LEN);
    if (expected_crc != actual_crc) {
        return -EBADMSG;
    }

    memset(packet, 0, sizeof(*packet));
    packet->msg_type = MSG_ROUTE_REQ;
    packet->flags = data[4];
    packet->src_id = source_id;
    packet->dst_id = MESH_BROADCAST_ID;
    packet->session_id = proto_get_u32_le(&data[6]);
    packet->seq = proto_get_u16_le(&data[10]);
    packet->ttl = data[5];
    packet->payload_len = declared_payload_len;
    memcpy(payload, &data[MESH_ROUTE_WAKE_ROUTE_HEADER_LEN], declared_payload_len);
    *payload_len = declared_payload_len;
    return 0;
}

static bool mesh_route_embedded_duplicate(const struct proto_packet *packet,
                                          uint32_t now_ms)
{
    uint32_t suppress_deadline_ms;

    if (!mesh_route_embedded_rx.valid || packet == NULL) {
        return false;
    }

    suppress_deadline_ms = mesh_route_embedded_rx.received_at_ms +
                           MESH_ROUTE_TEST_EMBEDDED_ROUTE_SUPPRESS_MS;
    if (uptime_deadline_reached(now_ms, suppress_deadline_ms)) {
        mesh_route_embedded_rx.valid = false;
        return false;
    }

    return mesh_route_embedded_rx.packet.msg_type == packet->msg_type &&
           mesh_route_embedded_rx.packet.src_id == packet->src_id &&
           mesh_route_embedded_rx.packet.dst_id == packet->dst_id &&
           mesh_route_embedded_rx.packet.session_id == packet->session_id &&
           mesh_route_embedded_rx.packet.seq == packet->seq;
}

static bool mesh_queue_embedded_route_request(
    const struct uwb_wake_claim_frame *claim,
    const uint8_t *suffix,
    size_t suffix_len,
    uint8_t link_quality)
{
    struct mesh_rx_pending pending = {0};
    size_t payload_len = 0u;
    uint32_t now_ms;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        claim == NULL || suffix == NULL || suffix_len == 0u ||
        claim->clicker_id == DEVICE_ID) {
        return false;
    }

    ret = mesh_route_wake_decode_suffix(suffix,
                                        suffix_len,
                                        claim->clicker_id,
                                        &pending.packet,
                                        pending.payload,
                                        sizeof(pending.payload),
                                        &payload_len);
    if (ret < 0) {
        high_debug_log_event("MESH_ROUTE_REQ_EMBEDDED_RX",
                             "phase=reject src=0x%016llx ret=%d suffix_len=%u",
                             (unsigned long long)claim->clicker_id,
                             ret,
                             (unsigned int)suffix_len);
        return false;
    }

    now_ms = k_uptime_get_32();
    if (mesh_route_embedded_duplicate(&pending.packet, now_ms)) {
        status_debug_note("DBG_EMBEDDED_ROUTE_REQ_DUP\n");
        return true;
    }

    pending.payload_len = (uint16_t)payload_len;
    pending.previous_hop_id = claim->clicker_id;
    pending.link_quality = link_quality;
    pending.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    pending.received_at_ms = now_ms;
    app_mesh_test_note_wake_event(&pending.packet,
                                  pending.previous_hop_id,
                                  pending.link_quality,
                                  pending.radio_channel);

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("embedded route request RX queue full; dropped src=0x%016llx seq=%u ret=%d",
                (unsigned long long)pending.packet.src_id,
                pending.packet.seq,
                ret);
        return false;
    }

    mesh_route_embedded_rx.valid = true;
    mesh_route_embedded_rx.packet = pending.packet;
    mesh_route_embedded_rx.received_at_ms = now_ms;
    mesh_route_embedded_reply_peer_id = claim->clicker_id;
    mesh_route_embedded_reply_hold_until_ms =
        now_ms + claim->wake_train_ends_in_ms + MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS;

    status_debug_note("DBG_EMBEDDED_ROUTE_REQ_RX\n");
    status_debug_printf("DBG_EMBEDDED_ROUTE_REQ src=0x%llx seq=%u hold_until=%u\n",
                        (unsigned long long)pending.packet.src_id,
                        pending.packet.seq,
                        mesh_route_embedded_reply_hold_until_ms);
    high_debug_log_event("MESH_ROUTE_REQ_EMBEDDED_RX",
                         "phase=queued src=0x%016llx seq=%u suffix_len=%u payload_len=%u hold_until=%u quality=%u",
                         (unsigned long long)pending.packet.src_id,
                         pending.packet.seq,
                         (unsigned int)suffix_len,
                         (unsigned int)payload_len,
                         mesh_route_embedded_reply_hold_until_ms,
                         link_quality);
    (void)mesh_submit_work(&mesh_rx_work);
    return true;
}

static void mesh_route_embedded_wait_before_reply(const struct mesh_outbound *route_reply)
{
    uint32_t now_ms;
    uint32_t delay_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        route_reply == NULL ||
        route_reply->packet.msg_type != MSG_ROUTE_REPLY ||
        mesh_route_embedded_reply_peer_id == 0u ||
        route_reply->next_hop_id != mesh_route_embedded_reply_peer_id) {
        return;
    }

    now_ms = k_uptime_get_32();
    delay_ms = uptime_ms_until_deadline(now_ms,
                                        mesh_route_embedded_reply_hold_until_ms);
    if (delay_ms > 0u) {
        status_debug_printf("DBG_EMBEDDED_ROUTE_REPLY_HOLD delay=%u peer=0x%llx\n",
                            delay_ms,
                            (unsigned long long)mesh_route_embedded_reply_peer_id);
        k_msleep(delay_ms);
    }

    mesh_route_embedded_reply_peer_id = 0u;
    mesh_route_embedded_reply_hold_until_ms = 0u;
}

static void mesh_schedule_route_waiting_retry_after(const char *reason, uint32_t delay_ms)
{
    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, delay_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_RETRY reason=%s delay=%u msg=0x%02x dst=0x%llx seq=%u active=%u\n",
                            reason == NULL ? "route-wait" : reason,
                            delay_ms,
                            mesh_route_waiting_tx.packet.msg_type,
                            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                            mesh_route_waiting_tx.packet.seq,
                            mesh_runtime.route_discovery.active ? 1u : 0u);
    }
    high_debug_log_event("MESH_ROUTE_WAIT",
                         "reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u retry_in_ms=%u",
                         reason == NULL ? "route-wait" : reason,
                         mesh_route_waiting_tx.packet.msg_type,
                         (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                         mesh_route_waiting_tx.packet.seq,
                         mesh_runtime.route_discovery.attempts,
                         delay_ms);
    LOG_INF("mesh route waiting: reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u retry_in_ms=%u",
            reason == NULL ? "route-wait" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            mesh_runtime.route_discovery.attempts,
            delay_ms);
}

static void mesh_schedule_route_waiting_retry(const char *reason)
{
    uint32_t now;
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }
    if (mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
        return;
    }

    now = k_uptime_get_32();
    deadline = mesh_runtime.route_discovery.active ?
               mesh_runtime.route_discovery.next_request_ms :
               now + REPORT_TX_RETRY_DELAY_MS;
    delay_ms = uptime_ms_until_deadline(now, deadline);
    mesh_schedule_route_waiting_retry_after(reason, delay_ms);
}

static uint32_t mesh_route_exhausted_retry_delay_ms(uint32_t random_value)
{
    return MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS +
           (random_value % (MESH_ROUTE_EXHAUSTED_RETRY_JITTER_MS + 1u));
}

static void mesh_schedule_route_waiting_exhausted_retry(const char *reason)
{
    uint32_t now_ms;
    uint32_t random_value;
    uint32_t delay_ms;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    mesh_relay_reset_route_discovery(&mesh_runtime);
    now_ms = k_uptime_get_32();
    random_value = sys_rand32_get();
    delay_ms = mesh_route_exhausted_retry_delay_ms(random_value);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_EXHAUSTED_RETRY reason=%s base=%u jitter=%u rand=%u delay=%u next=%u msg=0x%02x dst=0x%llx seq=%u lost=%u\n",
                            reason == NULL ? "route-exhausted" : reason,
                            MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS,
                            MESH_ROUTE_EXHAUSTED_RETRY_JITTER_MS,
                            random_value,
                            delay_ms,
                            now_ms + delay_ms,
                            mesh_route_waiting_tx.packet.msg_type,
                            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                            mesh_route_waiting_tx.packet.seq,
                            app_mesh_paused_delivery_lost_count(
                                &mesh_paused_delivery));
    }
    high_debug_log_event("MESH_ROUTE_WAIT_EXHAUSTED",
                         "reason=%s msg=0x%02x dst=0x%016llx seq=%u retry_in_ms=%u lost=%u",
                         reason == NULL ? "route-exhausted" : reason,
                         mesh_route_waiting_tx.packet.msg_type,
                         (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                         mesh_route_waiting_tx.packet.seq,
                         delay_ms,
                         app_mesh_paused_delivery_lost_count(
                             &mesh_paused_delivery));
    LOG_WRN("mesh route discovery exhausted; retained packet will retry: reason=%s msg=0x%02x dst=0x%016llx seq=%u retry_in_ms=%u",
            reason == NULL ? "route-exhausted" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            delay_ms);
    mesh_schedule_route_waiting_retry_after(reason == NULL ?
                                            "route-exhausted" : reason,
                                            delay_ms);
}

void mesh_report_resume_restored_outbox(const char *reason)
{
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_OUTBOX_RESTORE_RESUME reason=%s state=%u msg=0x%02x dst=0x%llx seq=%u retry=%u ack=%u now=%u\n",
                            reason == NULL ? "restore" : reason,
                            (unsigned int)mesh_runtime.pending.state,
                            mesh_runtime.pending.packet.msg_type,
                            (unsigned long long)mesh_runtime.pending.packet.dst_id,
                            mesh_runtime.pending.packet.seq,
                            mesh_runtime.pending.retry_after_ms,
                            mesh_runtime.pending.gateway_ack_deadline_ms,
                            k_uptime_get_32());
    }
    mesh_schedule_tx_timeout();
}

static const char *mesh_tx_handoff_reason_name(
    enum app_mesh_tx_handoff_reason reason)
{
    switch (reason) {
    case APP_MESH_TX_HANDOFF_REASON_RX_CONTROL:
        return "rx-control-handoff";
    case APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY:
        return "route-reply-handoff";
    case APP_MESH_TX_HANDOFF_REASON_NONE:
    default:
        return "handoff";
    }
}

static int mesh_tx_handoff_schedule_retry(
    enum app_mesh_tx_handoff_work work,
    enum app_mesh_tx_handoff_reason reason,
    uint32_t delay_ms,
    void *ctx)
{
    ARG_UNUSED(ctx);

    switch (work) {
    case APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING:
        mesh_schedule_route_waiting_retry_after(
            mesh_tx_handoff_reason_name(reason), delay_ms);
        return 0;
    case APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE:
        report_tx_schedule(delay_ms);
        return 0;
    default:
        return -EINVAL;
    }
}

static bool mesh_tx_handoff_gate_yields(
    enum app_mesh_tx_handoff_work work,
    bool queued_gateway_tx_pending,
    bool route_reply_handoff_active,
    bool rx_control_handoff_active,
    struct app_mesh_tx_handoff_result *result)
{
    const struct app_mesh_tx_handoff_ops ops = {
        .schedule_retry = mesh_tx_handoff_schedule_retry,
    };
    const struct app_mesh_tx_handoff_state state = {
        .queued_gateway_tx_pending = queued_gateway_tx_pending,
        .route_reply_handoff_active = route_reply_handoff_active,
        .rx_control_handoff_active = rx_control_handoff_active,
        .work = work,
        .retry_delay_ms = MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS,
    };

    return app_mesh_tx_handoff_gate_yield(&state, &ops, result);
}

static bool mesh_event_plan_debugs_channel5(enum mesh_event_plan_action action)
{
    return action == MESH_EVENT_PLAN_CLIP ||
           action == MESH_EVENT_PLAN_DEFER_CH5_ACTIVE ||
           action == MESH_EVENT_PLAN_SKIP_CH5_SCAN_GUARD ||
           action == MESH_EVENT_PLAN_REFRESH_CONTACT_CH5;
}

static bool mesh_debug_channel9_state(uint64_t peer_id,
                                      uint32_t *event_counter,
                                      bool *local_tx_slot)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return false;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (!entry->valid || entry->next_hop_id != peer_id) {
            continue;
        }
        if (event_counter != NULL) {
            *event_counter = entry->timing.event_counter;
        }
        if (local_tx_slot != NULL) {
            *local_tx_slot = mesh_event_timing_local_tx_slot(&entry->timing);
        }
        return true;
    }
    return false;
}

static void mesh_debug_channel5_preemption(const char *context,
                                           const char *reason,
                                           uint64_t peer_id,
                                           const struct mesh_channel5_requirements *requirements,
                                           const struct mesh_event_plan *plan,
                                           uint32_t now_ms)
{
    ARG_UNUSED(context);
    ARG_UNUSED(reason);
    ARG_UNUSED(peer_id);

    if (requirements == NULL || plan == NULL ||
        !mesh_event_plan_debugs_channel5(plan->action)) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH5_PREEMPT act=%u now=%u ch5=%u start=%u win=%u\n",
                            (unsigned int)plan->action,
                            now_ms,
                            requirements->next_required_scan_start_ms,
                            plan->start_ms,
                            plan->window_ms);
    }
}

static void mesh_debug_channel9_unavailable(const char *context,
                                            const char *reason,
                                            const struct proto_packet *packet,
                                            int ret,
                                            int select_ret,
                                            uint64_t selected_next_hop,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms)
{
    ARG_UNUSED(context);
    ARG_UNUSED(reason);
    ARG_UNUSED(selected_next_hop);

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || packet == NULL || plan == NULL) {
        return;
    }

    status_debug_printf("DBG_CH9_UNAVAIL ret=%d sel=%d act=%u now=%u start=%u win=%u\n",
                        ret,
                        select_ret,
                        (unsigned int)plan->action,
                        now_ms,
                        plan->start_ms,
                        plan->window_ms);

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];

        if (!entry->valid) {
            continue;
        }
        status_debug_printf("DBG_CH9_ENTRY i=%u usable=%u cnt=%u next=%u last=%u tx=%u\n",
                            i,
                            mesh_event_timing_usable(&entry->timing, now_ms) ? 1u : 0u,
                            entry->timing.event_counter,
                            entry->timing.next_event_time_ms,
                            entry->timing.last_successful_ch9_event_ms,
                            mesh_event_timing_local_tx_slot(&entry->timing) ? 1u : 0u);
    }
}

static bool mesh_role_uses_uwb_rx(void)
{
    return app_mesh_rx_policy_role_uses_uwb_rx(
        stage1_anchor_focused_rx_logs_enabled(),
        DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY,
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
        mesh_channel9_connection_count() > 0u);
}

static bool mesh_transport_paused(void)
{
    return atomic_get(&mesh_transport_paused_state) != 0;
}

static int mesh_transport_radio_start(const char *owner)
{
    int ret;

    if (mesh_transport_paused()) {
        return -ESHUTDOWN;
    }
    ret = radio_guard_uwb_start(owner);
    if (ret == 0 && mesh_transport_paused()) {
        /* Pause won the admission race before any PHY operation began. */
        radio_guard_uwb_stop();
        return -ESHUTDOWN;
    }
    return ret;
}

static bool mesh_rx_handoff_scan_rearm_allowed(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool allowed = app_mesh_rx_handoff_scan_rearm_allowed(&mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return allowed;
}

int mesh_node_comm_gateway_delivery_due_begin(bool *wait_for_scan_boundary)
{
    k_spinlock_key_t key;
    bool abort_scan = false;
    bool accepted;

    if (wait_for_scan_boundary == NULL) {
        return -EINVAL;
    }
    *wait_for_scan_boundary = false;
    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return -ENOTSUP;
    }

    key = k_spin_lock(&mesh_rx_handoff_lock);
    accepted = app_mesh_rx_handoff_request_scheduled_control(
        &mesh_rx_handoff, &abort_scan);
    if (accepted && abort_scan) {
        /*
         * Request abort only while this lock proves a scan owns the radio.
         * The pending gate prevents rearm; this path never clears the shared
         * abort bit, so it cannot consume a concurrent host-command request.
         */
        dwm3000_driver_request_receive_abort();
    }
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    if (!accepted) {
        return -EINVAL;
    }
    *wait_for_scan_boundary = abort_scan;
    return 0;
}

bool mesh_node_comm_gateway_delivery_due_pending(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool pending = app_mesh_rx_handoff_scheduled_control_pending(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return pending;
}

bool mesh_node_comm_gateway_delivery_due_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool ready = app_mesh_rx_handoff_scheduled_control_ready(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return ready;
}

bool mesh_node_comm_gateway_delivery_due_end(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool was_pending = app_mesh_rx_handoff_end_scheduled_control(
        &mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return was_pending;
}

static bool mesh_rx_handoff_begin_control(bool *abort_scan)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool started = app_mesh_rx_handoff_begin_control(&mesh_rx_handoff,
                                                      abort_scan);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return started;
}

static bool mesh_rx_handoff_control_ready(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);
    bool ready = app_mesh_rx_handoff_control_ready(&mesh_rx_handoff);

    k_spin_unlock(&mesh_rx_handoff_lock, key);
    return ready;
}

static void mesh_rx_handoff_end_control(void)
{
    k_spinlock_key_t key = k_spin_lock(&mesh_rx_handoff_lock);

    app_mesh_rx_handoff_end_control(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
}

static int mesh_rx_radio_start(const char *owner)
{
    k_spinlock_key_t key;
    bool accepted;
    int ret;

    if (!mesh_rx_handoff_scan_rearm_allowed()) {
        return -ECANCELED;
    }
    ret = mesh_transport_radio_start(owner);
    if (ret < 0) {
        return ret;
    }

    key = k_spin_lock(&mesh_rx_handoff_lock);
    accepted = app_mesh_rx_handoff_try_begin_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
    if (!accepted) {
        radio_guard_uwb_stop();
        return -ECANCELED;
    }
    return 0;
}

static void mesh_rx_radio_stop(void)
{
    k_spinlock_key_t key;

    radio_guard_uwb_stop();
    key = k_spin_lock(&mesh_rx_handoff_lock);
    app_mesh_rx_handoff_end_scan(&mesh_rx_handoff);
    k_spin_unlock(&mesh_rx_handoff_lock, key);
}

static int mesh_rx_handoff_wait_for_control(void)
{
    uint32_t deadline_ms = k_uptime_get_32() +
                           MESH_CONTROL_RX_HANDOFF_TIMEOUT_MS;

    while (!mesh_rx_handoff_control_ready() || radio_guard_uwb_busy()) {
        if (uptime_deadline_reached(k_uptime_get_32(), deadline_ms)) {
            return -EBUSY;
        }
        k_msleep(1);
    }
    return 0;
}

static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms)
{
    if (mesh_transport_paused() && work != &mesh_persistence_retry_work) {
        return -ESHUTDOWN;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    return k_work_reschedule_for_queue(&mesh_route_work_q, work, K_MSEC(delay_ms));
#else
    /*
     * Known queue gap: mesh_clicker does not yet enable the dedicated
     * mesh-route queue, so its node-communication work remains on sysworkq.
     * The gateway due-kick fix must not hide that role-specific limitation.
     */
    return k_work_reschedule(work, K_MSEC(delay_ms));
#endif
}

int mesh_route_work_reschedule(struct k_work_delayable *work, uint32_t delay_ms)
{
    if (work == NULL) {
        return -EINVAL;
    }

    return mesh_reschedule_delayable(work, delay_ms);
}

static int mesh_submit_work(struct k_work *work)
{
    if (mesh_transport_paused()) {
        return -ESHUTDOWN;
    }
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    return k_work_submit_to_queue(&mesh_route_work_q, work);
#else
    return k_work_submit(work);
#endif
}

static void mesh_schedule_async_route_request(uint64_t target_id, const char *reason)
{
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return;
    }

    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    mesh_route_discovery_target_id = target_id;
    mesh_route_discovery_reason = reason == NULL ? "async-route" : reason;
    mesh_route_discovery_pending = true;
    ret = mesh_reschedule_delayable(&mesh_route_discovery_work, 0u);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_SCHED target=0x%llx ret=%d reason=%s\n",
                            (unsigned long long)target_id,
                            ret,
                            mesh_route_discovery_reason);
    }
    k_mutex_unlock(&mesh_route_discovery_lock);
}

static void mesh_route_discovery_work_handler(struct k_work *work)
{
    uint64_t target_id;
    const char *reason;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    if (!mesh_route_discovery_pending) {
        k_mutex_unlock(&mesh_route_discovery_lock);
        return;
    }
    target_id = mesh_route_discovery_target_id;
    reason = mesh_route_discovery_reason == NULL ?
             "async-route" : mesh_route_discovery_reason;
    mesh_route_discovery_pending = false;
    mesh_route_discovery_target_id = 0u;
    mesh_route_discovery_reason = NULL;
    k_mutex_unlock(&mesh_route_discovery_lock);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_RUN target=0x%llx reason=%s\n",
                            (unsigned long long)target_id,
                            reason);
    }
    ret = mesh_request_route(target_id, reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_ASYNC_DONE target=0x%llx ret=%d reason=%s\n",
                            (unsigned long long)target_id,
                            ret,
                            reason);
    }
}

static int mesh_cancel_delayable(struct k_work_delayable *work)
{
    return k_work_cancel_delayable(work);
}

static bool mesh_pending_tx_blocks_uwb_rx(void)
{
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return false;
    }

    return !(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
             app_mesh_ch9_core_pending_allows_rx(
                 &mesh_runtime.pending,
                 mesh_relay_tx_active(&mesh_runtime)));
}

static uint32_t mesh_uwb_rx_window_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_WINDOW_MS :
           UWB_MESH_ANCHOR_RX_WINDOW_MS;
}

static uint32_t mesh_uwb_rx_idle_delay_ms(void)
{
    return DEVICE_ROLE == ROLE_GATEWAY ?
           UWB_MESH_GATEWAY_RX_IDLE_MS :
           UWB_MESH_ANCHOR_RX_INTERVAL_MS;
}

static bool mesh_next_channel9_activity_delay_ms(uint32_t now_ms,
                                                 uint32_t *delay_ms)
{
    bool found = false;
    uint32_t selected_delay_ms = 0u;

    (void)mesh_advance_all_channel9_timings_past(now_ms, "rx-delay");
    (void)mesh_expire_channel9_timings(now_ms, "rx-delay");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_timing timing = {0};
        uint32_t candidate_delay_ms;

        if (!mesh_channel9_next_required_activity(entry, &timing) ||
            !mesh_event_timing_usable(&timing, now_ms)) {
            continue;
        }
        candidate_delay_ms = uptime_ms_until_deadline(now_ms,
                                                      mesh_channel9_prepare_start_ms(&timing));
        if (!found || candidate_delay_ms < selected_delay_ms) {
            selected_delay_ms = candidate_delay_ms;
            found = true;
        }
    }
    if (found && delay_ms != NULL) {
        *delay_ms = selected_delay_ms;
    }
    return found;
}

static bool mesh_select_channel9_rx_event(uint32_t now_ms,
                                          struct mesh_event_plan *selected_plan,
                                          uint64_t *selected_peer,
                                          uint8_t *selected_index)
{
    struct mesh_channel5_requirements requirements;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL || selected_index == NULL) {
        return false;
    }

    (void)mesh_advance_all_channel9_timings_past(now_ms, "rx-select");
    (void)mesh_expire_channel9_timings(now_ms, "rx-select");
    mesh_fill_channel5_requirements(&requirements);
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_plan plan = {0};
        int ret;

        if (!entry->valid) {
            continue;
        }
        if (!mesh_event_timing_local_rx_slot(&entry->timing)) {
            continue;
        }
        ret = mesh_event_plan_channel9(&entry->timing, &requirements, now_ms, &plan);
        if (ret != PROTO_OK) {
            continue;
        }
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        mesh_debug_channel5_preemption("rx",
                                       "channel9-rx-select",
                                       entry->next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (plan.action != MESH_EVENT_PLAN_START &&
            plan.action != MESH_EVENT_PLAN_CLIP) {
            continue;
        }
        if (!selected || uptime_deadline_reached(selected_plan->start_ms, plan.start_ms)) {
            *selected_plan = plan;
            *selected_peer = entry->next_hop_id;
            *selected_index = i;
            selected = true;
        }
	    }
	    if (selected && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        const struct mesh_relay_event_timing_entry *entry =
	            &mesh_runtime.event_timings[*selected_index];

	        status_debug_printf("DBG_CH9_RX_SLOT idx=%u cnt=%u now=%u start=%u end=%u\n",
	                            *selected_index,
	                            entry->timing.event_counter,
	                            now_ms,
	                            selected_plan->start_ms,
	                            selected_plan->end_ms);
	    }
	    return selected;
}

static bool mesh_select_channel9_ack_tx_event(uint32_t now_ms,
                                               struct mesh_event_plan *selected_plan,
                                               uint64_t *selected_peer)
{
    struct mesh_channel5_requirements requirements;
    uint32_t selected_delay_ms = UINT32_MAX;
    bool selected = false;

    if (selected_plan == NULL || selected_peer == NULL ||
        !app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table)) {
        return false;
    }

    mesh_fill_channel5_requirements(&requirements);
    (void)mesh_expire_channel9_timings(now_ms, "ack-tx-select");
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        struct mesh_event_plan plan = {0};
        uint32_t delay_ms;
        uint64_t peer_id;
        int ret;

        if (!batch->valid || batch->count == 0u) {
            continue;
        }
        peer_id = batch->peer_id;
        (void)mesh_advance_channel9_timing_past(peer_id,
                                                now_ms,
                                                "ack-tx-select");
        if (!app_mesh_ch9_ack_table_pending_for_peer(&mesh_ch9_ack_table,
                                                      peer_id)) {
            continue;
        }
        if (!app_mesh_ch9_ack_table_retry_ready(&mesh_ch9_ack_table,
                                                 peer_id,
                                                 now_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf(
                    "DBG_CH9_ACK_BACKOFF peer=0x%llx wait=%u round=%u n=%u\n",
                    (unsigned long long)peer_id,
                    app_mesh_ch9_ack_table_retry_wait_ms(
                        &mesh_ch9_ack_table, peer_id, now_ms),
                    batch->retry_round,
                    batch->count);
            }
            continue;
        }
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   peer_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("ack-tx",
                                       "ch9-ack-batch-slot",
                                       peer_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note(ret == PROTO_ERR_STALE ?
                                  "DBG_CH9_ACK_TX_STALE\n" :
                                  "DBG_CH9_ACK_TX_WAIT\n");
                status_debug_printf("DBG_CH9_ACK_WAIT peer=0x%llx ret=%d act=%u now=%u start=%u n=%u\n",
                                    (unsigned long long)peer_id,
                                    ret,
                                    (unsigned int)plan.action,
                                    now_ms,
                                    plan.start_ms,
                                    batch->count);
            }
            if (ret == PROTO_ERR_STALE) {
                (void)mesh_ch9_ack_batch_clear_for_peer(peer_id,
                                                        "ack-tx-stale");
            } else if (ret == PROTO_ERR_BUSY &&
                       mesh_event_plan_is_policy_deferral(plan.action)) {
                uint32_t retry_delay_ms = 0u;

                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                peer_id,
                                                &mesh_event_stats);
                (void)app_mesh_ch9_ack_table_note_send_failure(
                    &mesh_ch9_ack_table,
                    peer_id,
                    now_ms,
                    sys_rand32_get(),
                    &retry_delay_ms);
                status_debug_printf(
                    "DBG_CH9_ACK_POLICY_DEFER peer=0x%llx delay=%u act=%u\n",
                    (unsigned long long)peer_id,
                    retry_delay_ms,
                    (unsigned int)plan.action);
            }
            continue;
        }

        delay_ms = uptime_ms_until_deadline(now_ms, plan.start_ms);
        if (!selected || delay_ms < selected_delay_ms) {
            *selected_plan = plan;
            *selected_peer = peer_id;
            selected_delay_ms = delay_ms;
            selected = true;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t event_counter = 0u;

            (void)mesh_debug_channel9_state(peer_id, &event_counter, NULL);
            status_debug_printf("DBG_CH9_ACK_SLOT peer=0x%llx cnt=%u now=%u start=%u end=%u n=%u\n",
                                (unsigned long long)peer_id,
                                event_counter,
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                batch->count);
        }
    }
    return selected;
}

static bool mesh_channel9_ack_pending_for_peer(uint64_t peer_id)
{
    return app_mesh_ch9_ack_table_pending_for_peer(&mesh_ch9_ack_table,
                                                    peer_id);
}

static bool mesh_channel9_next_required_activity(const struct mesh_relay_event_timing_entry *entry,
                                                 struct mesh_event_timing *timing)
{
    if (entry == NULL || !entry->valid || timing == NULL) {
        return false;
    }

    *timing = entry->timing;
    if (mesh_event_timing_local_rx_slot(timing) ||
        (mesh_event_timing_local_tx_slot(timing) &&
         mesh_channel9_ack_pending_for_peer(entry->next_hop_id))) {
        return true;
    }

    timing->next_event_time_ms += timing->event_interval_ms;
    timing->event_counter++;
    return mesh_event_timing_local_rx_slot(timing);
}

static uint32_t mesh_channel9_prepare_start_ms(const struct mesh_event_timing *timing)
{
    uint32_t retune_guard_ms;

    if (timing == NULL) {
        return 0u;
    }

    retune_guard_ms = timing->guard_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        retune_guard_ms < MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS) {
        retune_guard_ms = MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS;
    }

    if (timing->next_event_time_ms <= retune_guard_ms) {
        return 1u;
    }
    return timing->next_event_time_ms - retune_guard_ms;
}

static uint32_t mesh_next_channel9_rx_delay_ms(uint32_t now_ms)
{
    uint32_t delay_ms = mesh_uwb_rx_idle_delay_ms();

    if (mesh_next_channel9_activity_delay_ms(now_ms, &delay_ms)) {
        return delay_ms;
    }
    return delay_ms;
}

static uint32_t mesh_active_channel9_ch5_gap_window_ms(uint32_t now_ms)
{
    uint32_t delay_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        mesh_channel9_connection_count() == 0u ||
        !mesh_gateway_route_test_ch5_scan_has_capacity()) {
        return 0u;
    }

    delay_ms = mesh_next_channel9_rx_delay_ms(now_ms);
    return app_mesh_c5_connected_gap_window_ms(
        &(const struct app_mesh_c5_connected_gap_timing) {
            .next_channel9_delay_ms = delay_ms,
            .scan_cap_ms = DEVICE_ROLE == ROLE_GATEWAY ? UINT32_MAX :
                           MESH_ROUTE_TEST_CH5_GAP_SCAN_MS,
            .min_scan_ms = MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
            .retune_margin_ms = MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS,
        });
}

static void mesh_anchor_yield_idle_ch5_to_low_duty_scan(const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t next_delay_ms = 0u;
    bool channel9_pending =
        mesh_next_channel9_activity_delay_ms(now_ms, &next_delay_ms);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (mesh_anchor_rx_yield_log_next_ms == 0u ||
         uptime_deadline_reached(now_ms, mesh_anchor_rx_yield_log_next_ms) ||
         channel9_pending)) {
        mesh_anchor_rx_yield_log_next_ms = now_ms + MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS;
        status_debug_printf("DBG_ANCHOR_MESH_RX_YIELD reason=%s ch9=%u delay=%u scan_int=%u scan_win=%u wait=%u q=%u\n",
                            reason == NULL ? "idle" : reason,
                            channel9_pending ? 1u : 0u,
                            next_delay_ms,
                            anchor_uwb_scan_interval_ms,
                            ANCHOR_UWB_SCAN_RX_MS,
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }

    if (channel9_pending) {
        mesh_schedule_uwb_rx(next_delay_ms);
    } else {
        mesh_uwb_rx_active = false;
    }
}

static int mesh_defer_uwb_rx_rearm(uint32_t delay_ms)
{
    int ret;

    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    mesh_uwb_rx_rearm_delay_ms = delay_ms;
    mesh_uwb_rx_rearm_pending = true;
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);

    ret = mesh_submit_work(&mesh_uwb_rx_rearm_work);
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_REARM_DEFER delay=%u ret=%d active=%u\n",
                            delay_ms,
                            ret,
                            mesh_uwb_rx_active ? 1u : 0u);
    }
    return ret;
}

static bool mesh_take_deferred_uwb_rx_rearm(uint32_t *delay_ms)
{
    bool pending;

    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    pending = mesh_uwb_rx_rearm_pending;
    if (pending) {
        if (delay_ms != NULL) {
            *delay_ms = mesh_uwb_rx_rearm_delay_ms;
        }
        mesh_uwb_rx_rearm_pending = false;
    }
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);
    return pending;
}

static void mesh_clear_deferred_uwb_rx_rearm(void)
{
    k_mutex_lock(&mesh_uwb_rx_rearm_lock, K_FOREVER);
    mesh_uwb_rx_rearm_pending = false;
    k_mutex_unlock(&mesh_uwb_rx_rearm_lock);
    (void)k_work_cancel(&mesh_uwb_rx_rearm_work);
}

static void mesh_uwb_rx_rearm_work_handler(struct k_work *work)
{
    uint32_t delay_ms = 0u;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        return;
    }

    if (!mesh_take_deferred_uwb_rx_rearm(&delay_ms)) {
        return;
    }
    if (!mesh_uwb_rx_active || !mesh_role_uses_uwb_rx()) {
        return;
    }

    ret = mesh_reschedule_delayable(&mesh_uwb_rx_work, delay_ms);
    if (ret == -EBUSY) {
        (void)mesh_defer_uwb_rx_rearm(delay_ms);
        return;
    }
    if (ret < 0) {
        mesh_uwb_rx_active = false;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_REARM_RUN delay=%u ret=%d active=%u\n",
                            delay_ms,
                            ret,
                            mesh_uwb_rx_active ? 1u : 0u);
    }
}

static int mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    int ret;
    int defer_ret = 0;

    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }
    if (!mesh_rx_handoff_scan_rearm_allowed()) {
        mesh_uwb_rx_active = false;
        return -EAGAIN;
    }

    mesh_uwb_rx_active = true;
    ret = mesh_reschedule_delayable(&mesh_uwb_rx_work, delay_ms);
    if (ret == -EBUSY) {
        defer_ret = mesh_defer_uwb_rx_rearm(delay_ms);
    }
    if (ret < 0 && !(ret == -EBUSY && defer_ret >= 0)) {
        mesh_uwb_rx_active = false;
    }
    if (mesh_gateway_route_test_role()) {
        uint32_t now_ms = k_uptime_get_32();

        if (ret < 0 ||
            gateway_rx_diag_next_ms == 0u ||
            uptime_deadline_reached(now_ms, gateway_rx_diag_next_ms)) {
            gateway_rx_diag_next_ms = now_ms + 1000u;
            status_debug_printf("DBG_GATEWAY_RX_SCHEDULE delay=%u ret=%d defer=%d qrx=%u qtx=%u relay=%u wait=%u ack=%u adv_due=%u active=%u\n",
                                delay_ms,
                                ret,
                                defer_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                report_tx_queue_used(),
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                mesh_ch9_tx_pending.active ? 1u : 0u,
                                app_node_comm_gateway_route_refresh_due_ms(),
                                mesh_uwb_rx_active ? 1u : 0u);
        }
    }
    return ret == -EBUSY && defer_ret >= 0 ? 0 : ret;
}

static bool mesh_route_test_keeps_radio_idle_between_channel9_turns(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           (mesh_channel9_connection_count() > 0u ||
            mesh_ch9_tx_pending.active ||
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table));
}

bool mesh_anchor_connected_radio_active(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           mesh_route_test_keeps_radio_idle_between_channel9_turns();
}

static void mesh_release_radio_after_mesh_turn(bool channel9_turn,
                                               const char *reason)
{
    ARG_UNUSED(channel9_turn);

    if (mesh_route_test_keeps_radio_idle_between_channel9_turns()) {
        (void)dwm3000_driver_idle();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DWM_KEEP_IDLE reason=%s ch9=%u ack=%u qack=%u\n",
                                reason == NULL ? "mesh" : reason,
                                mesh_channel9_connection_count(),
                                mesh_ch9_tx_pending.active ? 1u : 0u,
                                app_mesh_ch9_ack_table_any_pending(
                                    &mesh_ch9_ack_table) ? 1u : 0u);
        }
        return;
    }

    (void)dwm3000_driver_standby();
}

void mesh_stop_role_scan(void)
{
    mesh_clear_deferred_uwb_rx_rearm();
    if (mesh_uwb_rx_active) {
        (void)mesh_cancel_delayable(&mesh_uwb_rx_work);
        mesh_uwb_rx_active = false;
    }
}

void mesh_restart_role_scan(void)
{
    int ret;

    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        return;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR && !anchor_uwb_busy) {
        ret = mesh_start_uwb_rx("anchor mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart anchor UWB mesh RX: %d", ret);
        }
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        ret = mesh_start_uwb_rx("gateway mesh restart");
        if (ret < 0) {
            LOG_WRN("mesh failed to restart gateway UWB mesh RX: %d", ret);
        }
    }
}

int mesh_transport_pause_preserving_queued(void)
{
    radio_guard_uwb_admission_pause();
    atomic_set(&mesh_transport_paused_state, 1);
    mesh_stop_role_scan();
    (void)k_work_cancel(&mesh_rx_work);
    (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
    (void)mesh_cancel_delayable(&mesh_c5_flood_work);
    (void)mesh_cancel_delayable(&mesh_route_discovery_work);
#if DEVICE_ROLE == ROLE_ANCHOR
    (void)mesh_cancel_delayable(&report_tx_work);
    (void)mesh_cancel_delayable(&mesh_route_request_action_work);
#endif
    return 0;
}

bool mesh_transport_quiesced(void)
{
    return mesh_transport_paused() &&
           !radio_guard_uwb_busy() &&
           !mesh_rx_response_active() &&
           atomic_get(&mesh_rx_handler_active_state) == 0;
}

void mesh_transport_resume(void)
{
    bool route_discovery_pending_now;

    if (!mesh_transport_paused()) {
        return;
    }
    atomic_set(&mesh_transport_paused_state, 0);
    radio_guard_uwb_admission_resume();

    if (k_msgq_num_used_get(&mesh_rx_msgq) > 0u) {
        (void)mesh_submit_work(&mesh_rx_work);
    }
    if (mesh_relay_tx_active(&mesh_runtime) ||
        mesh_ch9_tx_pending.active ||
        mesh_relay_result_bundle_pending(&mesh_runtime)) {
        (void)mesh_schedule_tx_timeout();
    }
#if DEVICE_ROLE == ROLE_ANCHOR
    if (report_tx_queue_used() > 0u) {
        report_tx_schedule(0u);
    }
    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    if (mesh_route_request_action_pending) {
        uint32_t delay_ms = app_mesh_route_request_defer_delay_ms(
            k_uptime_get_32(),
            mesh_route_request_action_tx.earliest_tx_ms);

        (void)mesh_reschedule_delayable(&mesh_route_request_action_work,
                                        delay_ms);
    }
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
#endif
    if (mesh_c5_flood_deferred.valid) {
        (void)mesh_reschedule_delayable(&mesh_c5_flood_work, 0u);
    }
    k_mutex_lock(&mesh_route_discovery_lock, K_FOREVER);
    route_discovery_pending_now = mesh_route_discovery_pending;
    k_mutex_unlock(&mesh_route_discovery_lock);
    if (route_discovery_pending_now) {
        (void)mesh_reschedule_delayable(&mesh_route_discovery_work, 0u);
    }
    mesh_restart_role_scan();
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_ch9_slot_tx_begin(struct mesh_ch9_slot_tx_context *ctx)
{
    int ret;

    if (ctx == NULL) {
        return -EINVAL;
    }
    if (ctx->active) {
        return 0;
    }

    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh channel9 batch TX");
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }

    ctx->uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_mesh_payload_mode();
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_SLOT_CONFIG ret=%d now=%u\n",
                            ret,
                            k_uptime_get_32());
    }
    if (ret < 0) {
        (void)dwm3000_driver_standby();
        mesh_report_note_anchor_uwb_awake_since(ctx->uwb_window_start_ms, 0u);
        radio_guard_uwb_stop();
        mesh_restart_role_scan();
        ctx->uwb_window_start_ms = -1;
        return ret;
    }

    ctx->active = true;
    return 0;
}

static void mesh_ch9_slot_tx_end(struct mesh_ch9_slot_tx_context *ctx)
{
    if (ctx == NULL || !ctx->active) {
        return;
    }

    mesh_release_radio_after_mesh_turn(true, "ch9-slot-tx");
    mesh_report_note_anchor_uwb_awake_since(ctx->uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    ctx->active = false;
    ctx->uwb_window_start_ms = -1;
}
#endif

static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *out,
                                                       const char *reason,
                                                       size_t *frame_len_out)
{
    struct mesh_outbound *tx = &mesh_send_scratch_tx;
    uint8_t *frame = mesh_send_scratch_frame;
    size_t frame_len = 0u;
    int ret;

    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }

    *tx = *out;
    mesh_outbound_refresh_age(tx, k_uptime_get_32());
    ret = uwb_mesh_frame_encode(NETWORK_ID,
                                DEVICE_ID,
                                tx->next_hop_id,
                                &tx->packet,
                                tx->payload,
                                frame,
                                UWB_MESH_MAX_FRAME_LEN,
                                &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        return -EINVAL;
    }

    if (tx->earliest_tx_ms != 0u) {
        mesh_wait_until_ms(tx->earliest_tx_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_SEND_AT now=%u earliest=%u msg=%02x len=%u\n",
                            k_uptime_get_32(),
                            tx->earliest_tx_ms,
                            tx->packet.msg_type,
                            (unsigned int)frame_len);
    }

    uint32_t send_begin_ms = k_uptime_get_32();

    ret = dwm3000_driver_send_frame(frame, frame_len, UWB_MESH_TX_TIMEOUT_MS);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                tx->packet.msg_type,
                (unsigned long long)tx->next_hop_id,
                (unsigned int)frame_len,
                ret);
        return ret;
    }
    app_watchdog_note_radio_progress();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t send_done_ms = k_uptime_get_32();

        status_debug_printf("DBG_CH9_TX_DONE now=%u dur=%u len=%u msg=%02x\n",
                            send_done_ms,
                            uptime_ms_until_deadline(send_begin_ms, send_done_ms),
                            (unsigned int)frame_len,
                            tx->packet.msg_type);
    }

    status_debug_tx_mesh_frame_sent_pulse();
    status_debug_uwb_tx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
    HIGH_DEBUG_COUNTER_INC(mesh_tx);
    if (tx->packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_gateway_ack_tx_pulse();
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_TX",
                             "dst=0x%016llx next=0x%016llx seq=%u channel=%u",
                             (unsigned long long)tx->packet.dst_id,
                             (unsigned long long)tx->next_hop_id,
                             tx->packet.seq,
                             UWB_CHANNEL_MESH_PAYLOAD);
    }
    high_debug_log_event("MESH_TX",
                         "reason=%s msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u age_ms=%u channel=%u frame_len=%u",
                         reason,
                         tx->packet.msg_type,
                         (unsigned long long)tx->packet.src_id,
                         (unsigned long long)tx->packet.dst_id,
                         (unsigned long long)tx->next_hop_id,
                         tx->packet.seq,
                         tx->packet.message_age_ms,
                         UWB_CHANNEL_MESH_PAYLOAD,
                         (unsigned int)frame_len);
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u age_ms=%u channel=%u frame_len=%u",
                reason,
                tx->packet.msg_type,
                (unsigned long long)tx->packet.src_id,
                (unsigned long long)tx->packet.dst_id,
                (unsigned long long)tx->next_hop_id,
                tx->packet.seq,
                tx->packet.ttl,
                tx->packet.message_age_ms,
                UWB_CHANNEL_MESH_PAYLOAD,
                (unsigned int)frame_len);
    }
    if (frame_len_out != NULL) {
        *frame_len_out = frame_len;
    }
    return 0;
}

static int mesh_send_outbound_with_release_on_channel(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    bool *rf_started_out,
    uint8_t forced_radio_channel)
{
    struct mesh_outbound *tx = &mesh_send_scratch_tx;
    uint8_t *frame = mesh_send_scratch_frame;
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    bool channel5_extended_control = false;
    uint8_t radio_channel;
    int ret;

    if (rf_started_out != NULL) {
        *rf_started_out = false;
    }
    if (out == NULL) {
        return -EINVAL;
    }
    radio_channel = forced_radio_channel == 0u ? out->radio_channel :
                    forced_radio_channel;
    if (radio_channel != UWB_CHANNEL_WAKE_CONTACT &&
        radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        LOG_ERR("mesh TX rejected invalid radio channel: channel=%u reason=%s",
                radio_channel,
                reason == NULL ? "unknown" : reason);
        return -EINVAL;
    }
    if (!mesh_coordinator_mesh_work_allowed(reason)) {
        return -EBUSY;
    }
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
        out->earliest_tx_ms != 0u) {
        uint32_t guard_start_ms = out->earliest_tx_ms;

        if (guard_start_ms > MESH_CH9_TX_CONFIG_GUARD_MS) {
            guard_start_ms -= MESH_CH9_TX_CONFIG_GUARD_MS;
        }
        if (!uptime_deadline_reached(k_uptime_get_32(), guard_start_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_PRE_GUARD_WAIT now=%u guard=%u earliest=%u msg=%02x\n",
                                    k_uptime_get_32(),
                                    guard_start_ms,
                                    out->earliest_tx_ms,
                                    out->packet.msg_type);
            }
            mesh_wait_until_ms(guard_start_ms);
        }
        if (!mesh_coordinator_mesh_work_allowed(reason)) {
            return -EBUSY;
        }
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    *tx = *out;
    tx->radio_channel = radio_channel;
    mesh_outbound_refresh_age(tx, k_uptime_get_32());

    ret = uwb_mesh_frame_encode(NETWORK_ID,
                                DEVICE_ID,
                                tx->next_hop_id,
                                &tx->packet,
                                tx->payload,
                                frame,
                                UWB_MESH_MAX_FRAME_LEN,
                                &frame_len);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh frame encode failed for %s: %d", reason, ret);
        ret = -EINVAL;
        goto out_unlock;
    }
    channel5_extended_control =
        tx->radio_channel != UWB_CHANNEL_MESH_PAYLOAD &&
        app_mesh_c5_control_uses_extended_phr(
            tx->packet.msg_type,
            frame_len,
            MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN);

    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh UWB TX");
    if (ret < 0) {
        mesh_restart_role_scan();
        goto out_unlock;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
          dwm3000_driver_configure_mesh_payload_mode() :
          channel5_extended_control ?
          dwm3000_driver_configure_wake_mesh_control_mode() :
          dwm3000_driver_configure_wake_mode();
    if (tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_CONFIG ret=%d now=%u earliest=%u msg=%02x\n",
                                ret,
                                k_uptime_get_32(),
                                tx->earliest_tx_ms,
                                tx->packet.msg_type);
        }
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_C5_TX_CONFIG ret=%d ext=%u len=%u msg=%02x reason=%s\n",
                            ret,
                            channel5_extended_control ? 1u : 0u,
                            (unsigned int)frame_len,
                            tx->packet.msg_type,
                            reason == NULL ? "mesh" : reason);
    }
    if (ret == 0) {
        if (tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            tx->earliest_tx_ms != 0u) {
            mesh_wait_until_ms(tx->earliest_tx_ms);
        }
        if (tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_SEND_AT now=%u earliest=%u msg=%02x len=%u\n",
                                k_uptime_get_32(),
                                tx->earliest_tx_ms,
                                tx->packet.msg_type,
                                (unsigned int)frame_len);
        }
        if (rf_started_out != NULL) {
            *rf_started_out = true;
        }
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_MESH_TX_TIMEOUT_MS);
    }
    if (ret == 0 && tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        if (release_policy == MESH_RADIO_RELEASE_IDLE) {
            (void)dwm3000_driver_idle();
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_RELEASE_IDLE msg=%02x seq=%u reason=%s\n",
                                    tx->packet.msg_type,
                                    tx->packet.seq,
                                    reason == NULL ? "mesh" : reason);
            }
        } else {
            mesh_release_radio_after_mesh_turn(true, reason);
        }
    } else {
        (void)dwm3000_driver_standby();
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB TX failed for %s: msg=0x%02x next=0x%016llx len=%u ret=%d",
                reason,
                tx->packet.msg_type,
                (unsigned long long)tx->next_hop_id,
                (unsigned int)frame_len,
                ret);
        goto out_unlock;
    }

    app_watchdog_note_radio_progress();
    status_debug_tx_mesh_frame_sent_pulse();
    status_debug_uwb_tx_channel_pulse(tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                                      UWB_CHANNEL_MESH_PAYLOAD :
                                      UWB_CHANNEL_WAKE_CONTACT);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        tx->packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_SENT now=%u seq=%u\n",
                            k_uptime_get_32(),
                            tx->packet.seq);
    }
    HIGH_DEBUG_COUNTER_INC(mesh_tx);
    if (tx->packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_gateway_ack_tx_pulse();
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_TX",
                             "dst=0x%016llx next=0x%016llx seq=%u channel=%u",
                             (unsigned long long)tx->packet.dst_id,
                             (unsigned long long)tx->next_hop_id,
                             tx->packet.seq,
                             tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                             UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT);
    }
    high_debug_log_event("MESH_TX",
                         "reason=%s msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u age_ms=%u channel=%u frame_len=%u",
                         reason,
                         tx->packet.msg_type,
                         (unsigned long long)tx->packet.src_id,
                         (unsigned long long)tx->packet.dst_id,
                         (unsigned long long)tx->next_hop_id,
                         tx->packet.seq,
                         tx->packet.message_age_ms,
                         tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                         UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
                         (unsigned int)frame_len);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        tx->packet.msg_type == MSG_GATEWAY_ROUTE_REQ) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_RETURN now=%u seq=%u\n",
                            k_uptime_get_32(),
                            tx->packet.seq);
    }
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        LOG_INF("mesh UWB TX %s: msg=0x%02x src=0x%016llx dst=0x%016llx next=0x%016llx seq=%u ttl=%u age_ms=%u channel=%u frame_len=%u",
                reason,
                tx->packet.msg_type,
                (unsigned long long)tx->packet.src_id,
                (unsigned long long)tx->packet.dst_id,
                (unsigned long long)tx->next_hop_id,
                tx->packet.seq,
                tx->packet.ttl,
                tx->packet.message_age_ms,
                tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
                UWB_CHANNEL_MESH_PAYLOAD : UWB_CHANNEL_WAKE_CONTACT,
                (unsigned int)frame_len);
    }
    ret = 0;

out_unlock:
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

static int mesh_send_outbound_with_release(
    const struct mesh_outbound *out,
    const char *reason,
    enum mesh_radio_release_policy release_policy,
    bool *rf_started_out)
{
    return mesh_send_outbound_with_release_on_channel(out,
                                                      reason,
                                                      release_policy,
                                                      rf_started_out,
                                                      0u);
}

int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    return mesh_send_outbound_with_release(out,
                                           reason,
                                           MESH_RADIO_RELEASE_STANDBY,
                                           NULL);
}

static int mesh_send_outbound_keep_channel9_awake(const struct mesh_outbound *out,
                                                  const char *reason,
                                                  bool *rf_started_out)
{
    if (out == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return -EINVAL;
    }

    return mesh_send_outbound_with_release(out,
                                           reason,
                                           MESH_RADIO_RELEASE_IDLE,
                                           rf_started_out);
}

static int mesh_send_c5_control_attempt(
    const struct mesh_outbound *out,
    uint8_t purpose,
    enum mesh_c5_control_send_mode mode,
    const char *reason,
    bool *rf_started)
{
    const struct mesh_outbound *tx = out;
    uint64_t peer_id;
    uint64_t wake_target_id;
    bool active_exchange = false;
    uint32_t now_ms;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (out == NULL || purpose == 0u ||
        (mode != MESH_C5_CONTROL_WAKE_IF_NEEDED &&
         mode != MESH_C5_CONTROL_ACCEPTED_EXCHANGE)) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    /* The synchronous caller retains its immutable envelope through return. */
    peer_id = mesh_id_is_unicast(tx->next_hop_id) ? tx->next_hop_id : 0u;
    now_ms = k_uptime_get_32();
    if (peer_id != 0u) {
        active_exchange = mesh_c5_contact_peer_active(peer_id, now_ms);
    }

    if (mode == MESH_C5_CONTROL_ACCEPTED_EXCHANGE && !active_exchange) {
        LOG_WRN("mesh C5 control send rejected without accepted exchange: msg=0x%02x next=0x%016llx purpose=%s reason=%s",
                tx->packet.msg_type,
                (unsigned long long)tx->next_hop_id,
                mesh_c5_contact_purpose_name(purpose),
                reason == NULL ? "c5-control" : reason);
        ret = -ENOTCONN;
        goto out_unlock;
    }

    if (mode == MESH_C5_CONTROL_WAKE_IF_NEEDED && !active_exchange) {
        wake_target_id = peer_id != 0u ? peer_id : MESH_BROADCAST_ID;
        ret = mesh_send_route_wake_train(wake_target_id, NULL, NULL,
                                         purpose, reason);
        if (ret < 0) {
            mesh_restart_role_scan();
            LOG_WRN("mesh C5 control wake train failed: msg=0x%02x next=0x%016llx ret=%d reason=%s",
                    tx->packet.msg_type,
                    (unsigned long long)tx->next_hop_id,
                    ret,
                    reason == NULL ? "c5-control" : reason);
            goto out_unlock;
        }
    }

    if (peer_id != 0u) {
        mesh_c5_contact_exchange(peer_id,
                                 purpose,
                                 mesh_c5_exchange_expires_at(purpose),
                                 reason == NULL ? "c5-control" : reason);
    }

    ret = mesh_send_outbound_with_release_on_channel(
        tx,
        reason == NULL ? "c5-control" : reason,
        MESH_RADIO_RELEASE_STANDBY,
        rf_started,
        UWB_CHANNEL_WAKE_CONTACT);
    if (ret == 0 && peer_id != 0u) {
        mesh_c5_contact_exchange(peer_id,
                                 purpose,
                                 mesh_c5_exchange_expires_at(purpose),
                                 reason == NULL ? "c5-control" : reason);
    }
out_unlock:
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

int mesh_send_c5_control(const struct mesh_outbound *out,
                         uint8_t purpose,
                         enum mesh_c5_control_send_mode mode,
                         const char *reason)
{
    return mesh_send_c5_control_attempt(out, purpose, mode, reason, NULL);
}

static uint32_t mesh_c5_flood_now_ms(void *ctx)
{
    ARG_UNUSED(ctx);

    return k_uptime_get_32();
}

static void mesh_c5_flood_sleep_until_ms(uint32_t due_ms, void *ctx)
{
    int64_t uwb_window_start_ms;

    while (!uptime_deadline_reached(k_uptime_get_32(), due_ms)) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t remaining_ms = uptime_ms_until_deadline(now_ms, due_ms);
        uint32_t listen_ms = MIN(remaining_ms, MESH_C5_FLOOD_DELAY_LISTEN_SLICE_MS);
        enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
        bool valid_mesh_frame = false;
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        int ret;

        if (listen_ms == 0u) {
            break;
        }

        mesh_stop_role_scan();
        ret = mesh_transport_radio_start("mesh C5 flood delay listen");
        if (ret < 0) {
            mesh_restart_role_scan();
            mesh_wait_until_ms(due_ms);
            return;
        }
        uwb_window_start_ms = k_uptime_get();
        ret = dwm3000_driver_configure_wake_mesh_control_mode();
        if (ret == 0) {
            ret = dwm3000_driver_receive_frame_continuous(listen_ms,
                                                          mesh_uwb_rx_frame,
                                                          sizeof(mesh_uwb_rx_frame),
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
        }
        (void)dwm3000_driver_standby();
        mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
        radio_guard_uwb_stop();
        mesh_restart_role_scan();

        if (ret == 0) {
            (void)mesh_queue_from_frame_at_internal(mesh_uwb_rx_frame,
                                                    frame_len,
                                                    quality,
                                                    UWB_CHANNEL_WAKE_CONTACT,
                                                    k_uptime_get_32(),
                                                    NULL,
                                                    0u,
                                                    false,
                                                    &valid_mesh_frame,
                                                    NULL);
            if (valid_mesh_frame) {
                (void)mesh_process_queued_rx_now("c5-flood-delay-listen");
            }
        } else if (ret < 0 && ret != -ETIMEDOUT &&
                   IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_C5_FLOOD_DELAY_RX_CONTINUE ret=%d fail=%u rem=%u\n",
                                ret,
                                (unsigned int)rx_failure,
                                remaining_ms);
        }
    }

    ARG_UNUSED(ctx);
}

static bool mesh_c5_flood_defer_active_cb(void *ctx)
{
    const struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    struct app_mesh_coordinator_decision coordinator_decision;
    struct app_mesh_c5_flood_priority_state state = {
        .response_priority = flood_ctx != NULL && flood_ctx->response_priority,
    };
    uint32_t now_ms;

    mesh_coordinator_decide_now("c5-flood", &coordinator_decision);
    if (!coordinator_decision.mesh_work_allowed) {
        return true;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        state.anchor_busy = anchor_uwb_window_active();
        state.survey_busy = mesh_report_anchor_survey_discovery_is_pending();
    }

    now_ms = k_uptime_get_32();
    state.gateway_ch5_preempt = mesh_gateway_route_test_preempt_active(now_ms);

    return app_mesh_c5_flood_should_defer(&state);
}

static bool mesh_c5_flood_quiet_cb(uint32_t sniff_ms, void *ctx)
{
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    int ret;

    ARG_UNUSED(ctx);

    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh C5 flood politeness");
    if (ret < 0) {
        mesh_restart_role_scan();
        return false;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        ret = dwm3000_driver_receive_frame(sniff_ms,
                                           mesh_uwb_rx_frame,
                                           sizeof(mesh_uwb_rx_frame),
                                           &frame_len,
                                           NULL,
                                           NULL);
    }
    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();

    return ret == -ETIMEDOUT;
}

static uint32_t mesh_c5_flood_random_u32(void *ctx)
{
    ARG_UNUSED(ctx);

    return sys_rand32_get();
}

static int mesh_c5_flood_send_cb(const struct mesh_outbound *out, void *ctx)
{
    struct mesh_c5_flood_tx_context *flood_ctx = ctx;
    bool rf_started = false;
    int ret;

    ret = mesh_send_outbound_with_release(
        out, "bounded-c5-flood", MESH_RADIO_RELEASE_STANDBY, &rf_started);
    if (rf_started && flood_ctx != NULL &&
        flood_ctx->rf_started_out != NULL) {
        *flood_ctx->rf_started_out = true;
    }
    return ret;
}

static bool mesh_c5_flood_same_packet(const struct mesh_outbound *left,
                                      const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == right->packet.msg_type &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq;
}

static uint32_t mesh_c5_flood_deferred_retry_ms(
    const struct mesh_outbound *out)
{
    const struct app_mesh_rf_retry_key retry_key =
        mesh_rf_retry_packet_key(
            &out->packet,
            APP_MESH_RF_RETRY_OPERATION_CONTROL_FLOOD);

    return mesh_rf_retry_next_delay_ms(
        &mesh_c5_flood_deferred.rf_retry,
        &retry_key,
        APP_MESH_RF_RETRY_POLICY_CONTROL_FLOOD,
        "c5-flood-deferred");
}

static int mesh_c5_flood_store_deferred(const struct mesh_outbound *out,
                                        uint8_t purpose,
                                        const char *reason,
                                        bool response_priority)
{
    uint32_t retry_ms;

    if (out == NULL) {
        return -EINVAL;
    }
    if (mesh_c5_flood_deferred.valid) {
        if (mesh_c5_flood_same_packet(&mesh_c5_flood_deferred.outbound, out)) {
            return 0;
        }
        if (!response_priority || mesh_c5_flood_deferred.response_priority) {
            LOG_WRN("mesh C5 flood defer slot busy: queued=0x%02x rejected=0x%02x priority=%u",
                    mesh_c5_flood_deferred.outbound.packet.msg_type,
                    out->packet.msg_type,
                    response_priority ? 1u : 0u);
            return -ENOSPC;
        }
        LOG_WRN("mesh C5 priority flood displaced non-priority work: queued=0x%02x priority=0x%02x",
                mesh_c5_flood_deferred.outbound.packet.msg_type,
                out->packet.msg_type);
    }
    mesh_c5_flood_deferred.outbound = *out;
    mesh_c5_flood_deferred.purpose = purpose;
    mesh_c5_flood_deferred.reason = reason;
    mesh_c5_flood_deferred.response_priority = response_priority;
    mesh_c5_flood_deferred.retry_count = 0u;
    app_mesh_rf_retry_reset(&mesh_c5_flood_deferred.rf_retry);
    mesh_c5_flood_deferred.queued_at_ms = k_uptime_get_32();
    mesh_c5_flood_deferred.valid = true;
    retry_ms = mesh_c5_flood_deferred_retry_ms(out);
    mesh_c5_flood_deferred.retry_count = 1u;
    (void)mesh_reschedule_delayable(&mesh_c5_flood_work, retry_ms);
    return 0;
}

static bool mesh_c5_flood_purpose_is_priority(uint8_t purpose)
{
    return purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD ||
           purpose == C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH ||
           purpose == C5_CONTACT_PURPOSE_COLLECTION_EACK_FLOOD;
}

static bool mesh_c5_flood_destination_valid(const struct mesh_outbound *out)
{
    bool broadcast;
    bool targeted_command;

    if (out == NULL) {
        return false;
    }

    broadcast = out->packet.dst_id == MESH_BROADCAST_ID &&
                out->next_hop_id == MESH_BROADCAST_ID;
    targeted_command = mesh_id_is_unicast(out->packet.dst_id) &&
                       mesh_id_is_unicast(out->next_hop_id) &&
                       (out->packet.msg_type == MSG_COMMAND ||
                        out->packet.msg_type == MSG_SURVEY_PAIR_PREPARE);
    return broadcast || targeted_command;
}

static int mesh_send_c5_flood_now(const struct mesh_outbound *out,
                                  uint8_t purpose,
                                  const char *reason,
                                  bool send_wake_train,
                                  bool response_priority,
                                  bool single_opportunity,
                                  const struct app_mesh_command_orchestrator *command_orchestrator,
                                  struct app_mesh_flood_result *result,
                                  bool *rf_started_out)
{
    struct mesh_outbound tx;
    struct app_mesh_flood_result aggregate_result = {0};
    struct mesh_c5_flood_tx_context flood_ctx = {
        .rf_started_out = rf_started_out,
        .response_priority = response_priority,
    };
    struct app_mesh_flood_ops ops = {
        .now_ms = mesh_c5_flood_now_ms,
        .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
        .defer_active = mesh_c5_flood_defer_active_cb,
        .c5_quiet = mesh_c5_flood_quiet_cb,
        .random_u32 = mesh_c5_flood_random_u32,
        .send = mesh_c5_flood_send_cb,
        .ctx = &flood_ctx,
    };
    const char *log_reason = reason == NULL ? "c5-flood" : reason;
    uint16_t attempt_count;
    int ret;

    if (rf_started_out != NULL) {
        *rf_started_out = false;
    }
    if (out == NULL || purpose == 0u ||
        !mesh_c5_flood_destination_valid(out) ||
        out->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        out->packet.msg_type == MSG_ROUTE_REQ) {
        return -EINVAL;
    }

    tx = *out;
    tx.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    attempt_count = 1u + tx.flood_retry_count;

    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        return -EAGAIN;
    }

    for (uint16_t attempt = 0u; attempt < attempt_count; attempt++) {
        struct app_mesh_flood_result attempt_result = {0};
        uint32_t first_tx_ms = tx.earliest_tx_ms != 0u ? tx.earliest_tx_ms :
                               k_uptime_get_32();
        uint32_t wake_start_ms = first_tx_ms;

        if (send_wake_train && first_tx_ms > WAKE_ADV_MS) {
            wake_start_ms = first_tx_ms - WAKE_ADV_MS;
        }
        if (!uptime_deadline_reached(k_uptime_get_32(), wake_start_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_C5_FLOOD_WAIT now=%u wake=%u first=%u msg=%02x attempt=%u/%u reason=%s\n",
                                    k_uptime_get_32(),
                                    wake_start_ms,
                                    first_tx_ms,
                                    tx.packet.msg_type,
                                    attempt + 1u,
                                    attempt_count,
                                    log_reason);
            }
            mesh_wait_until_ms(wake_start_ms);
        }
        if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
            ret = aggregate_result.sent_count > 0u ? 0 : -EAGAIN;
            break;
        }

        if (send_wake_train) {
            ret = mesh_send_route_wake_train(MESH_BROADCAST_ID, NULL, NULL,
                                             purpose, reason);
            if (ret < 0) {
                mesh_restart_role_scan();
                LOG_WRN("mesh C5 flood wake train failed: msg=0x%02x ret=%d reason=%s",
                        tx.packet.msg_type,
                        ret,
                        log_reason);
                return ret;
            }
        }

        ret = single_opportunity ?
            app_mesh_flood_send_opportunity(&tx, &ops, &attempt_result) :
            command_orchestrator == NULL ?
            app_mesh_command_orchestrator_serialize_flood(&tx,
                                                          &ops,
                                                          &attempt_result) :
            app_mesh_command_orchestrator_send_flood(command_orchestrator,
                                                      &ops,
                                                      &attempt_result);
        aggregate_result.sent_count =
            UINT8_MAX - aggregate_result.sent_count < attempt_result.sent_count ?
            UINT8_MAX : aggregate_result.sent_count + attempt_result.sent_count;
        aggregate_result.busy_skip_count =
            UINT8_MAX - aggregate_result.busy_skip_count < attempt_result.busy_skip_count ?
            UINT8_MAX : aggregate_result.busy_skip_count + attempt_result.busy_skip_count;
        aggregate_result.deferred_count =
            UINT8_MAX - aggregate_result.deferred_count < attempt_result.deferred_count ?
            UINT8_MAX : aggregate_result.deferred_count + attempt_result.deferred_count;
        if (attempt == 0u) {
            aggregate_result.first_due_ms = attempt_result.first_due_ms;
        }
        aggregate_result.last_due_ms = attempt_result.last_due_ms;
        if (ret != 0) {
            ret = aggregate_result.sent_count > 0u ? 0 : ret;
            break;
        }
        if (attempt + 1u < attempt_count) {
            tx.earliest_tx_ms = k_uptime_get_32() + FLOOD_POST_ROOT_GUARD_MS;
        }
    }

    if (result != NULL) {
        *result = aggregate_result;
    }
    if (ret == 0) {
        LOG_DBG("mesh bounded C5 flood sent: msg=0x%02x sent=%u busy_skip=%u defer=%u retry=%u reason=%s",
                tx.packet.msg_type,
                aggregate_result.sent_count,
                aggregate_result.busy_skip_count,
                aggregate_result.deferred_count,
                out->flood_retry_count,
                log_reason);
    }
    return ret;
}

int mesh_try_send_c5_flood(const struct mesh_outbound *out,
                           uint8_t purpose,
                           const char *reason,
                           bool *rf_started)
{
    struct app_mesh_flood_result result = {0};
    bool priority = mesh_c5_flood_purpose_is_priority(purpose);

    if (rf_started != NULL) {
        *rf_started = false;
    }
    return mesh_send_c5_flood_now(out, purpose, reason, true, priority,
                                  false, NULL, &result, rf_started);
}

int mesh_try_send_c5_flood_resume(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    struct app_mesh_flood_progress *progress,
    struct app_mesh_flood_result *result,
    bool *rf_started)
{
    struct mesh_outbound tx;
    struct mesh_c5_flood_tx_context flood_ctx = {
        .rf_started_out = rf_started,
        .response_priority = mesh_c5_flood_purpose_is_priority(purpose),
    };
    struct app_mesh_flood_ops ops = {
        .now_ms = mesh_c5_flood_now_ms,
        .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
        .defer_active = mesh_c5_flood_defer_active_cb,
        .c5_quiet = mesh_c5_flood_quiet_cb,
        .random_u32 = mesh_c5_flood_random_u32,
        .send = mesh_c5_flood_send_cb,
        .ctx = &flood_ctx,
    };
    uint32_t first_tx_ms;
    uint32_t wake_start_ms;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (out == NULL || progress == NULL || result == NULL || purpose == 0u ||
        !mesh_c5_flood_destination_valid(out) ||
        out->radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        out->packet.msg_type == MSG_ROUTE_REQ || out->flood_retry_count != 0u) {
        return -EINVAL;
    }

    tx = *out;
    tx.radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        *result = progress->result;
        return -EAGAIN;
    }

    first_tx_ms = progress->initialized ? progress->due_ms :
                  tx.earliest_tx_ms != 0u ? tx.earliest_tx_ms :
                  k_uptime_get_32();
    wake_start_ms = first_tx_ms > WAKE_ADV_MS ?
                    first_tx_ms - WAKE_ADV_MS : first_tx_ms;
    if (!uptime_deadline_reached(k_uptime_get_32(), wake_start_ms)) {
        mesh_wait_until_ms(wake_start_ms);
    }
    if (mesh_c5_flood_defer_active_cb(&flood_ctx)) {
        *result = progress->result;
        return -EAGAIN;
    }

    ret = mesh_send_route_wake_train(MESH_BROADCAST_ID, NULL, NULL,
                                     purpose, reason);
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }
    ret = app_mesh_flood_send_bounded_resume(&tx, &ops, progress, result);
    if (ret == 0 &&
        (!progress->complete ||
         progress->next_opportunity != app_mesh_flood_repeat_limit() ||
         result->sent_count != app_mesh_flood_repeat_limit())) {
        return -EAGAIN;
    }
    return ret;
}

int mesh_try_send_c5_flood_view(const struct app_mesh_outbound_view *view,
                                uint8_t purpose,
                                const char *reason,
                                bool *rf_started)
{
    struct mesh_outbound out_storage;
    struct mesh_outbound *out = &out_storage;
    bool control_handoff_started = false;
    bool abort_scan = false;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (view == NULL || view->packet == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > sizeof(out->payload) ||
        view->packet->payload_len != view->payload_len) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        control_handoff_started = mesh_rx_handoff_begin_control(&abort_scan);
        if (!control_handoff_started) {
            ret = -EBUSY;
            goto out_unlock;
        }
        mesh_stop_role_scan();
        if (abort_scan) {
            dwm3000_driver_request_receive_abort();
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            status_debug_printf("DBG_CONTROL_RX_HANDOFF_TIMEOUT abort=%u busy=%u\n",
                                abort_scan ? 1u : 0u,
                                radio_guard_uwb_busy() ? 1u : 0u);
            goto out_unlock;
        }
        status_debug_printf("DBG_CONTROL_RX_HANDOFF_READY abort=%u\n",
                            abort_scan ? 1u : 0u);
    }
    memset(out, 0, sizeof(*out));
    out->packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out->payload, view->payload, view->payload_len);
    }
    out->payload_len = view->payload_len;
    out->radio_channel = view->radio_channel;
    out->next_hop_id = view->next_hop_id;
    out->queued_at_ms = view->queued_at_ms;
    out->earliest_tx_ms = view->earliest_tx_ms;
    out->flood_retry_count = view->flood_retry_count;
    ret = mesh_send_c5_flood_now(out,
                                 purpose,
                                 reason,
                                 true,
                                 mesh_c5_flood_purpose_is_priority(purpose),
                                 true,
                                 NULL,
                                 NULL,
                                 rf_started);

out_unlock:
    if (control_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

int mesh_try_send_control_response_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    bool *rf_started)
{
    struct mesh_outbound out;
    bool control_handoff_started = false;
    bool abort_scan = false;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (view == NULL || view->packet == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > sizeof(out.payload) ||
        view->packet->payload_len != view->payload_len ||
        view->packet->msg_type != MSG_GATEWAY_ACK ||
        view->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !mesh_id_is_unicast(view->next_hop_id)) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_c5_control_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }
    if (DEVICE_ROLE == ROLE_GATEWAY) {
        control_handoff_started = mesh_rx_handoff_begin_control(&abort_scan);
        if (!control_handoff_started) {
            ret = -EBUSY;
            goto out_unlock;
        }
        mesh_stop_role_scan();
        if (abort_scan) {
            dwm3000_driver_request_receive_abort();
        }
        ret = mesh_rx_handoff_wait_for_control();
        if (ret < 0) {
            status_debug_printf("DBG_RESPONSE_RX_HANDOFF_TIMEOUT abort=%u busy=%u\n",
                                abort_scan ? 1u : 0u,
                                radio_guard_uwb_busy() ? 1u : 0u);
            goto out_unlock;
        }
        status_debug_printf("DBG_RESPONSE_RX_HANDOFF_READY abort=%u\n",
                            abort_scan ? 1u : 0u);
    }

    memset(&out, 0, sizeof(out));
    out.packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out.payload, view->payload, view->payload_len);
    }
    out.payload_len = view->payload_len;
    out.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    out.next_hop_id = view->next_hop_id;
    out.queued_at_ms = view->queued_at_ms;
    /* A response is unscheduled; stale event timing must never delay it. */
    out.earliest_tx_ms = 0u;
    ret = mesh_send_outbound_keep_channel9_awake(
        &out, reason == NULL ? "control-response" : reason, rf_started);

out_unlock:
    if (control_handoff_started) {
        mesh_rx_handoff_end_control();
        mesh_restart_role_scan();
    }
    k_mutex_unlock(&mesh_c5_control_scratch_lock);
    return ret;
}

static bool mesh_node_comm_packet_identity_matches(
    const struct proto_packet *left,
    const struct proto_packet *right)
{
    return left != NULL && right != NULL &&
           left->msg_type == right->msg_type &&
           left->src_id == right->src_id &&
           left->dst_id == right->dst_id &&
           left->session_id == right->session_id &&
           left->seq == right->seq;
}

int mesh_try_send_reliable_uplink_view(
    const struct app_mesh_outbound_view *view,
    const char *reason,
    bool *rf_started,
    bool *gateway_confirmed,
    uint32_t *scheduled_retry_delay_ms)
{
    /*
     * The communication facade keeps compact frozen records.  This one
     * compatibility scratch expands only the request currently handed to the
     * legacy mesh backend, avoiding another 1 KiB system-workqueue frame.
     * app_node_comm serializes backend calls, so it cannot be re-entered.
     */
    static struct mesh_outbound out;
    bool pending_exact;
    bool policy_deferred = false;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (gateway_confirmed != NULL) {
        *gateway_confirmed = false;
    }
    if (scheduled_retry_delay_ms != NULL) {
        *scheduled_retry_delay_ms = 0u;
    }
    if (view == NULL || view->packet == NULL || rf_started == NULL ||
        gateway_confirmed == NULL || scheduled_retry_delay_ms == NULL ||
        (view->payload_len != 0u && view->payload == NULL) ||
        view->payload_len > APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN ||
        view->packet->payload_len != view->payload_len ||
        !mesh_id_is_unicast(view->packet->dst_id)) {
        return -EINVAL;
    }

    memset(&out, 0, sizeof(out));
    out.packet = *view->packet;
    if (view->payload_len != 0u) {
        memcpy(out.payload, view->payload, view->payload_len);
    }
    out.payload_len = view->payload_len;
    out.radio_channel = view->radio_channel;
    out.next_hop_id = view->next_hop_id;
    out.queued_at_ms = view->queued_at_ms;
    out.earliest_tx_ms = view->earliest_tx_ms;
    out.flood_retry_count = view->flood_retry_count;

    ret = mesh_start_tracked_tx_with_retry(
        &out,
        reason == NULL ? "node-comm-reliable-uplink" : reason,
        scheduled_retry_delay_ms,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL),
        NULL,
        rf_started,
        &policy_deferred);
    if (policy_deferred) {
        /* Channel-5 contention remains a randomized exponential retry. */
        *scheduled_retry_delay_ms = 0u;
    }
    pending_exact = mesh_relay_tx_active(&mesh_runtime) &&
        mesh_node_comm_packet_identity_matches(
            &mesh_runtime.pending.packet, view->packet);
    *gateway_confirmed = ret == 0 && *rf_started && !pending_exact;
    return ret;
}

int mesh_cancel_reliable_uplink(const struct proto_packet *packet)
{
    bool cancelled = false;

    if (packet == NULL) {
        return -EINVAL;
    }
    if (mesh_relay_tx_active(&mesh_runtime) &&
        mesh_node_comm_packet_identity_matches(
            &mesh_runtime.pending.packet, packet)) {
        mesh_relay_cancel_tx(&mesh_runtime);
        (void)app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
        cancelled = true;
    }
    if (mesh_route_waiting_tx_valid &&
        mesh_node_comm_packet_identity_matches(
            &mesh_route_waiting_tx.packet, packet)) {
        mesh_route_waiting_tx_valid = false;
        cancelled = true;
    }
    return cancelled ? 0 : -ENOENT;
}

int mesh_send_c5_flood(const struct mesh_outbound *out,
                       uint8_t purpose,
                       const char *reason,
                       bool *sent_now)
{
    bool attempted = false;
    bool priority = mesh_c5_flood_purpose_is_priority(purpose);
    int ret = mesh_try_send_c5_flood(out, purpose, reason, &attempted);

    if (sent_now != NULL) {
        *sent_now = attempted;
    }
    if (!attempted && mesh_send_failure_retryable(ret)) {
        ret = mesh_c5_flood_store_deferred(out, purpose, reason, priority);
    }
    return ret;
}

static int mesh_gateway_control_send_flood(
    void *ctx,
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    struct app_mesh_flood_result *result)
{
    ARG_UNUSED(ctx);
    return mesh_send_c5_flood_now(&orchestrator->gateway_flow.outbound,
                                  C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                                  reason,
                                  true,
                                  true,
                                  false,
                                  orchestrator,
                                  result,
                                  NULL);
}

int mesh_send_gateway_command_flood(
    const struct app_mesh_command_orchestrator *orchestrator,
    const char *reason,
    bool *sent_now)
{
    return app_node_comm_gateway_control_send(orchestrator, reason, sent_now);
}

static int mesh_send_c5_flood_response(const struct mesh_outbound *out,
                                       uint8_t purpose,
                                       const char *reason)
{
    struct app_mesh_flood_result result = {0};
    int ret;

    ret = mesh_send_c5_flood_now(out, purpose, reason, true, true, false, NULL,
                                 &result, NULL);
    if (ret == -EAGAIN && (result.sent_count == 0u)) {
        mesh_c5_flood_store_deferred(out, purpose, reason, true);
    }
    return ret;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_same_tlv_value(const uint8_t *lhs,
                                size_t lhs_len,
                                const uint8_t *rhs,
                                size_t rhs_len,
                                uint8_t type)
{
    const uint8_t *lhs_value = NULL;
    const uint8_t *rhs_value = NULL;
    uint8_t lhs_value_len = 0u;
    uint8_t rhs_value_len = 0u;

    if (tlv_find(lhs, lhs_len, type, &lhs_value, &lhs_value_len) != PROTO_OK ||
        tlv_find(rhs, rhs_len, type, &rhs_value, &rhs_value_len) != PROTO_OK ||
        lhs_value_len != rhs_value_len) {
        return false;
    }

    return memcmp(lhs_value, rhs_value, lhs_value_len) == 0;
}

static bool mesh_route_reply_ack_matches(const struct mesh_outbound *route_reply,
                                         const struct mesh_frame_parse_context *parsed)
{
    if (route_reply == NULL ||
        parsed == NULL ||
        parsed->packet.msg_type != MSG_ROUTE_REPLY_ACK ||
        parsed->packet.src_id != route_reply->next_hop_id ||
        parsed->packet.dst_id != DEVICE_ID ||
        parsed->previous_hop_id != route_reply->next_hop_id ||
        parsed->packet.session_id != route_reply->packet.session_id) {
        return false;
    }

    return mesh_same_tlv_value(route_reply->payload,
                               route_reply->payload_len,
                               parsed->payload,
                               parsed->payload_len,
                               TLV_INITIATOR_ID) &&
           mesh_same_tlv_value(route_reply->payload,
                               route_reply->payload_len,
                               parsed->payload,
                               parsed->payload_len,
                               TLV_RESPONDER_ID) &&
           mesh_same_tlv_value(route_reply->payload,
                               route_reply->payload_len,
                               parsed->payload,
                               parsed->payload_len,
                               TLV_FLOOD_EPOCH_ID) &&
           mesh_same_tlv_value(route_reply->payload,
                               route_reply->payload_len,
                               parsed->payload,
                               parsed->payload_len,
                               TLV_REPLY_NONCE) &&
           mesh_same_tlv_value(route_reply->payload,
                               route_reply->payload_len,
                               parsed->payload,
                               parsed->payload_len,
                               TLV_METRIC_CRC);
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_listen_for_route_reply_ack(const struct mesh_outbound *route_reply,
                                           uint8_t attempt)
{
    uint8_t *frame = mesh_route_reply_ack_frame;
    struct mesh_frame_parse_context *parsed = &mesh_route_reply_ack_parsed;
    int64_t uwb_window_start_ms = -1;
    uint32_t deadline_ms;
    uint32_t latest_deadline_ms = 0u;
    bool captured = false;
    uint8_t capture_quality = 0u;
    int last_ret = -ETIMEDOUT;
    int ret;

    if (route_reply == NULL ||
        route_reply->packet.msg_type != MSG_ROUTE_REPLY ||
        !mesh_id_is_unicast(route_reply->next_hop_id)) {
        return -EINVAL;
    }
    if (!mesh_coordinator_mesh_work_allowed("route-reply-ack-listen")) {
        return -EBUSY;
    }

    ret = k_mutex_lock(&mesh_route_reply_ack_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    mesh_c5_contact_exchange(route_reply->next_hop_id,
                             C5_CONTACT_PURPOSE_ROUTE_REPLY,
                             k_uptime_get_32() + RREP_ACK_TIMEOUT_MS,
                             "route-reply-ack-listen");
    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh route reply ACK RX");
    if (ret < 0) {
        mesh_restart_role_scan();
        goto out_unlock;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=start next=0x%016llx seq=%u attempt=%u timeout_ms=%u",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         attempt,
                         RREP_ACK_TIMEOUT_MS);
    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        uint32_t listen_start_ms = k_uptime_get_32();

        deadline_ms = listen_start_ms + RREP_ACK_TIMEOUT_MS;
        latest_deadline_ms = u32_saturating_add(listen_start_ms,
                                                RREP_ACK_ATTEMPT_MAX_MS);
        if (latest_deadline_ms == 0u) {
            latest_deadline_ms = 1u;
        }
        while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms)) {
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
            size_t frame_len = 0u;
            uint8_t quality = 0u;

            if (remaining_ms == 0u) {
                break;
            }
            memset(parsed, 0, sizeof(*parsed));

            ret = dwm3000_driver_receive_frame_continuous(remaining_ms,
                                                          frame,
                                                          UWB_MESH_MAX_FRAME_LEN,
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
            last_ret = ret;
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                     "phase=rx-fail next=0x%016llx seq=%u attempt=%u ret=%d rx_failure=%u",
                                     (unsigned long long)route_reply->next_hop_id,
                                     route_reply->packet.seq,
                                     attempt,
                                     ret,
                                     (unsigned int)rx_failure);
                continue;
            }

            {
                bool embedded_route_frame = false;
                bool click_priority = false;

                if (mesh_handle_channel5_wake_claim(frame,
                                                    frame_len,
                                                    quality,
                                                    &embedded_route_frame,
                                                    &click_priority)) {
                    uint32_t preempted_at_ms = k_uptime_get_32();

                    if (DEVICE_ROLE == ROLE_ANCHOR && click_priority) {
                        last_ret = -EAGAIN;
                        status_debug_note("DBG_ROUTE_REPLY_ACK_CLICK_PREEMPT\n");
                        high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                             "phase=click-preempt next=0x%016llx seq=%u attempt=%u",
                                             (unsigned long long)route_reply->next_hop_id,
                                             route_reply->packet.seq,
                                             attempt);
                        break;
                    }
                    deadline_ms = app_mesh_route_reply_ack_deadline_after_preemption(
                        preempted_at_ms,
                        RREP_ACK_TIMEOUT_MS,
                        latest_deadline_ms);
                    status_debug_note("DBG_ROUTE_REPLY_ACK_PREEMPT_EXTEND\n");
                    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                         "phase=c5-preempt-click next=0x%016llx seq=%u attempt=%u extended_deadline=%u latest_deadline=%u",
                                         (unsigned long long)route_reply->next_hop_id,
                                         route_reply->packet.seq,
                                         attempt,
                                         deadline_ms,
                                         latest_deadline_ms);
                    continue;
                }
            }

            ret = uwb_mesh_frame_decode(frame,
                                        frame_len,
                                        NETWORK_ID,
                                        DEVICE_ID,
                                        &parsed->previous_hop_id,
                                        &parsed->packet,
                                        parsed->payload,
                                        sizeof(parsed->payload),
                                        &parsed->payload_len);
            if (ret != PROTO_OK || parsed->payload_len > UINT8_MAX) {
                high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                     "phase=reject next=0x%016llx seq=%u attempt=%u len=%u quality=%u decode_ret=%d",
                                     (unsigned long long)route_reply->next_hop_id,
                                     route_reply->packet.seq,
                                     attempt,
                                     (unsigned int)frame_len,
                                     quality,
                                     ret);
                continue;
            }

            if (!mesh_route_reply_ack_matches(route_reply, parsed)) {
                LOG_INF("mesh route reply ACK listen ignored unrelated mesh frame: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                        parsed->packet.msg_type,
                        (unsigned long long)parsed->packet.src_id,
                        (unsigned long long)parsed->packet.dst_id,
                        (unsigned long long)parsed->previous_hop_id,
                        parsed->packet.seq,
                        quality);
                continue;
            }

            capture_quality = quality;
            captured = true;
            status_debug_note("DBG_ROUTE_REPLY_ACK_RX\n");
            high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                                 "phase=capture next=0x%016llx seq=%u ack_seq=%u attempt=%u quality=%u",
                                 (unsigned long long)route_reply->next_hop_id,
                                 route_reply->packet.seq,
                                 parsed->packet.seq,
                                 attempt,
                                 quality);
            break;
        }
    } else {
        last_ret = ret;
        high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                             "phase=config-fail next=0x%016llx seq=%u attempt=%u ret=%d",
                             (unsigned long long)route_reply->next_hop_id,
                             route_reply->packet.seq,
                             attempt,
                             ret);
    }

    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();

    if (captured) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_CONSUMED seq=%u quality=%u\n",
                                route_reply->packet.seq,
                                capture_quality);
        }
        ret = 0;
        goto out_unlock;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=timeout next=0x%016llx seq=%u attempt=%u last_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         attempt,
                         last_ret);
    ret = last_ret;

out_unlock:
    k_mutex_unlock(&mesh_route_reply_ack_scratch_lock);
    return ret;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_send_route_reply_burst(const struct mesh_outbound *route_reply,
                                       uint8_t attempt,
                                       bool apply_turnaround_delay)
{
    uint8_t repeat_count = 1u;
    uint8_t sent_count = 0u;
    int last_ret = -EINVAL;

    if (route_reply == NULL) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        DEVICE_ROLE == ROLE_GATEWAY &&
        apply_turnaround_delay &&
        mesh_id_is_unicast(route_reply->next_hop_id)) {
        repeat_count = MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_COUNT;
        LOG_INF("mesh route-reply turnaround delay: next=0x%016llx delay_ms=%u repeats=%u gap_ms=%u attempt=%u",
                (unsigned long long)route_reply->next_hop_id,
                MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS,
                repeat_count,
                MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS,
                attempt);
        k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS);
    }

    for (uint8_t i = 0u; i < repeat_count; i++) {
        if (i > 0u) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS);
        }

        last_ret = mesh_send_c5_control(route_reply,
                                        C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                        MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                        i == 0u ? "route-reply" :
                                                  "route-reply-repeat");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_TX_REPEAT attempt=%u idx=%u ret=%d sent=%u\n",
                                attempt,
                                i,
                                last_ret,
                                sent_count + (last_ret == 0 ? 1u : 0u));
        }
        if (last_ret == 0) {
            sent_count++;
        }
    }

    return sent_count > 0u ? 0 : last_ret;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_send_route_reply_train_to_hop(const struct mesh_outbound *route_reply)
{
    int last_ret = -EINVAL;

    if (route_reply == NULL) {
        return -EINVAL;
    }

    for (uint8_t attempt = 0u; attempt <= RREP_RETRY_COUNT_PER_HOP; attempt++) {
        struct app_mesh_route_reply_ack_attempt_state attempt_state = {
            .attempt = attempt,
            .max_retries = RREP_RETRY_COUNT_PER_HOP,
        };
        struct app_mesh_route_reply_ack_attempt_result attempt_result;

        last_ret = mesh_send_route_reply_burst(route_reply, attempt, attempt == 0u);
        attempt_state.send_ret = last_ret;
        if (last_ret == 0) {
            last_ret = mesh_listen_for_route_reply_ack(route_reply, attempt);
            attempt_state.listen_attempted = true;
            attempt_state.listen_ret = last_ret;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ATTEMPT attempt=%u send=%d listen=%d listened=%u\n",
                                attempt,
                                attempt_state.send_ret,
                                attempt_state.listen_ret,
                                attempt_state.listen_attempted ? 1u : 0u);
        }

        app_mesh_route_reply_ack_decide_attempt(&attempt_state, &attempt_result);
        if (attempt_result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_SUCCESS) {
            return attempt_result.return_ret;
        }
        last_ret = attempt_result.return_ret;
        if (attempt_result.note_retry) {
            mesh_relay_note_route_reply_retry(&mesh_runtime);
        }
        if (attempt_result.action == APP_MESH_ROUTE_REPLY_ACK_ATTEMPT_RETRY) {
            continue;
        }
        break;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=failed next=0x%016llx seq=%u attempts=%u last_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         route_reply->packet.seq,
                         (unsigned int)(RREP_RETRY_COUNT_PER_HOP + 1u),
                         last_ret);
    return last_ret;
}
#endif

static int mesh_send_route_reply_train(const struct mesh_outbound *route_reply,
                                       bool backup_valid,
                                       uint64_t backup_next_hop_id,
                                       uint64_t *acked_next_hop_id)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound *backup_route_reply = &mesh_route_reply_backup_scratch;
    struct app_mesh_route_reply_ack_backup_state backup_state;
    struct app_mesh_route_reply_ack_backup_result backup_result;
    int primary_ret;
    int ret;

    if (acked_next_hop_id != NULL) {
        *acked_next_hop_id = 0u;
    }
    if (route_reply == NULL) {
        return -EINVAL;
    }

    ret = mesh_send_route_reply_train_to_hop(route_reply);
    if (ret == 0) {
        if (acked_next_hop_id != NULL) {
            *acked_next_hop_id = route_reply->next_hop_id;
        }
        mesh_c5_contact_clear("route-reply-acked");
        return 0;
    }

    primary_ret = ret;
    backup_state.primary_ret = primary_ret;
    backup_state.backup_valid = backup_valid;
    backup_state.primary_next_hop_id = route_reply->next_hop_id;
    backup_state.backup_next_hop_id = backup_next_hop_id;
    app_mesh_route_reply_ack_decide_backup(&backup_state, &backup_result);
    if (!backup_result.try_backup) {
        mesh_c5_contact_clear(backup_result.clear_reason);
        return ret;
    }

    ret = k_mutex_lock(&mesh_route_reply_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    *backup_route_reply = *route_reply;
    backup_route_reply->next_hop_id = backup_result.backup_next_hop_id;
    if (backup_result.note_retry) {
        mesh_relay_note_route_reply_retry(&mesh_runtime);
    }
    high_debug_log_event("MESH_ROUTE_REPLY_ACK_RX",
                         "phase=backup primary=0x%016llx backup=0x%016llx seq=%u primary_ret=%d",
                         (unsigned long long)route_reply->next_hop_id,
                         (unsigned long long)backup_result.backup_next_hop_id,
                         route_reply->packet.seq,
                         primary_ret);

    ret = mesh_send_route_reply_train_to_hop(backup_route_reply);
    if (ret == 0 && acked_next_hop_id != NULL) {
        *acked_next_hop_id = backup_route_reply->next_hop_id;
    }
    k_mutex_unlock(&mesh_route_reply_scratch_lock);
    mesh_c5_contact_clear(ret == 0 ? "route-reply-backup-acked" :
                                   "route-reply-backup-failed");
    return ret;
#else
    ARG_UNUSED(route_reply);
    ARG_UNUSED(backup_valid);
    ARG_UNUSED(backup_next_hop_id);
    ARG_UNUSED(acked_next_hop_id);
    return -ENOTSUP;
#endif
}

static bool mesh_payload_find_u32(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t type,
                                  uint32_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    if (value == NULL ||
        tlv_find(payload, payload_len, type, &tlv_value, &tlv_len) != PROTO_OK ||
        tlv_len != sizeof(uint32_t)) {
        return false;
    }

    *value = proto_get_u32_le(tlv_value);
    return true;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_payload_find_u64(const uint8_t *payload,
                                  size_t payload_len,
                                  uint8_t type,
                                  uint64_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    if (value == NULL ||
        tlv_find(payload, payload_len, type, &tlv_value, &tlv_len) != PROTO_OK ||
        tlv_len != sizeof(uint64_t)) {
        return false;
    }

    *value = proto_get_u64_le(tlv_value);
    return true;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t mesh_ch9_next_batch_id(void)
{
    if (mesh_ch9_batch_next_id == 0u) {
        mesh_ch9_batch_next_id =
            sys_rand32_get() ^ k_uptime_get_32() ^ (uint32_t)DEVICE_ID;
    }
    mesh_ch9_batch_next_id++;
    if (mesh_ch9_batch_next_id == 0u) {
        mesh_ch9_batch_next_id = 1u;
    }
    return mesh_ch9_batch_next_id;
}
#endif

static int mesh_ch9_batch_metadata_parse(const uint8_t *payload,
                                         size_t payload_len,
                                         struct mesh_ch9_batch_metadata *metadata)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (metadata == NULL) {
        return PROTO_ERR_ARG;
    }
    memset(metadata, 0, sizeof(*metadata));

    ret = tlv_find(payload,
                   payload_len,
                   TLV_MESH_CH9_BATCH_ID,
                   &value,
                   &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint32_t)) {
        return PROTO_ERR_MALFORMED;
    }

    metadata->batch_id = proto_get_u32_le(value);
    metadata->present = true;

    ret = tlv_find(payload,
                   payload_len,
                   TLV_MESH_CH9_BATCH_FLAGS,
                   &value,
                   &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(uint8_t)) {
        return PROTO_ERR_MALFORMED;
    }

    metadata->flags = value[0];
    metadata->final_packet =
        (metadata->flags & MESH_CH9_BATCH_FLAG_FINAL) != 0u;
    return PROTO_OK;
}

static int mesh_payload_copy_without_ch9_batch_metadata(uint8_t *dst,
                                                        size_t dst_cap,
                                                        size_t *dst_len,
                                                        const uint8_t *src,
                                                        size_t src_len)
{
    size_t src_offset = 0u;
    size_t out_len = 0u;

    if (dst == NULL || dst_len == NULL || (src == NULL && src_len > 0u)) {
        return PROTO_ERR_ARG;
    }

    while (src_offset < src_len) {
        uint8_t type;
        uint8_t len;
        size_t tlv_len;

        if (src_len - src_offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = src[src_offset];
        len = src[src_offset + 1u];
        tlv_len = (size_t)len + 2u;
        if (src_len - src_offset < tlv_len) {
            return PROTO_ERR_MALFORMED;
        }

        if (type != TLV_MESH_CH9_BATCH_ID &&
            type != TLV_MESH_CH9_BATCH_FLAGS) {
            if (dst_cap - out_len < tlv_len) {
                return PROTO_ERR_NO_SPACE;
            }
            memcpy(&dst[out_len], &src[src_offset], tlv_len);
            out_len += tlv_len;
        }
        src_offset += tlv_len;
    }

    *dst_len = out_len;
    return PROTO_OK;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_outbound_set_ch9_batch_metadata(struct mesh_outbound *out,
                                                uint32_t batch_id,
                                                bool final_packet)
{
    size_t payload_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    k_mutex_lock(&mesh_ch9_batch_payload_lock, K_FOREVER);
    ret = mesh_payload_copy_without_ch9_batch_metadata(
        mesh_ch9_batch_payload_scratch,
        sizeof(mesh_ch9_batch_payload_scratch),
        &payload_len,
        out->payload,
        out->payload_len);
    if (ret != PROTO_OK) {
        goto out_unlock;
    }

    ret = tlv_append_u32(mesh_ch9_batch_payload_scratch,
                         sizeof(mesh_ch9_batch_payload_scratch),
                         &payload_len,
                         TLV_MESH_CH9_BATCH_ID,
                         batch_id);
    if (ret != PROTO_OK) {
        goto out_unlock;
    }
    ret = tlv_append_u8(mesh_ch9_batch_payload_scratch,
                        sizeof(mesh_ch9_batch_payload_scratch),
                        &payload_len,
                        TLV_MESH_CH9_BATCH_FLAGS,
                        final_packet ? MESH_CH9_BATCH_FLAG_FINAL : 0u);
    if (ret != PROTO_OK) {
        goto out_unlock;
    }

    memcpy(out->payload, mesh_ch9_batch_payload_scratch, payload_len);
    out->payload_len = (uint16_t)payload_len;
    out->packet.payload_len = (uint16_t)payload_len;
    ret = PROTO_OK;

out_unlock:
    k_mutex_unlock(&mesh_ch9_batch_payload_lock);
    return ret;
}
#endif

static int mesh_outbound_clear_ch9_batch_metadata(struct mesh_outbound *out)
{
    size_t payload_len = 0u;
    int ret;

    if (out == NULL) {
        return PROTO_ERR_ARG;
    }

    k_mutex_lock(&mesh_ch9_batch_payload_lock, K_FOREVER);
    ret = mesh_payload_copy_without_ch9_batch_metadata(
        mesh_ch9_batch_payload_scratch,
        sizeof(mesh_ch9_batch_payload_scratch),
        &payload_len,
        out->payload,
        out->payload_len);
    if (ret != PROTO_OK) {
        goto out_unlock;
    }

    memcpy(out->payload, mesh_ch9_batch_payload_scratch, payload_len);
    out->payload_len = (uint16_t)payload_len;
    out->packet.payload_len = (uint16_t)payload_len;
    ret = PROTO_OK;

out_unlock:
    k_mutex_unlock(&mesh_ch9_batch_payload_lock);
    return ret;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static uint8_t mesh_requeue_ch9_batch_candidates(struct mesh_outbound *candidates,
                                                 uint8_t start_index,
                                                 uint8_t count,
                                                 const char *reason)
{
    uint8_t requeued = 0u;

    if (candidates == NULL) {
        return 0u;
    }

    for (uint8_t i = start_index; i < count; i++) {
        struct mesh_outbound clean = candidates[i];
        int strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
        int ret;

        ret = queue_anchor_report(strip_ret == PROTO_OK ? &clean : &candidates[i]);
        if (ret == 0) {
            requeued++;
        } else {
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_REQUEUE idx=%u count=%u ret=%d strip=%d seq=%u r=%s\n",
                                i,
                                count,
                                ret,
                                strip_ret,
                                candidates[i].packet.seq,
                                reason == NULL ? "batch" : reason);
        }
    }
    return requeued;
}
#endif

static int mesh_ack_payload_packet_id_summary(const uint8_t *payload,
                                              size_t payload_len,
                                              uint8_t *count,
                                              uint32_t *first_id,
                                              uint32_t *last_id)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t id_count;
    int ret;

    if (count == NULL || first_id == NULL || last_id == NULL) {
        return PROTO_ERR_ARG;
    }
    *count = 0u;
    *first_id = 0u;
    *last_id = 0u;

    ret = tlv_find(payload, payload_len, TLV_MESH_ACK_PACKET_ID_LIST, &value, &value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if ((value_len % sizeof(uint32_t)) != 0u) {
        return PROTO_ERR_MALFORMED;
    }

    id_count = value_len / sizeof(uint32_t);
    *count = id_count;
    if (id_count > 0u) {
        *first_id = proto_get_u32_le(value);
        *last_id = proto_get_u32_le(&value[(id_count - 1u) * sizeof(uint32_t)]);
    }
    return PROTO_OK;
}

static void mesh_ch9_ack_batch_queue(const struct mesh_outbound *ack,
                                     const struct mesh_rx_pending *rx)
{
    struct app_mesh_ch9_ack_batch_entry entry = {0};
    enum app_mesh_ch9_ack_queue_result queue_result =
        APP_MESH_CH9_ACK_QUEUE_ADDED;
    uint32_t packet_id = 0u;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        ack == NULL ||
        rx == NULL ||
        rx->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        (ack->packet.msg_type != MSG_GATEWAY_ACK &&
         ack->packet.msg_type != MSG_MESH_HOP_ACK)) {
        return;
    }

    entry.seq = rx->packet.seq;
    entry.session_id = rx->packet.session_id;
    entry.has_packet_id = mesh_payload_find_u32(rx->payload,
                                                rx->payload_len,
                                                TLV_MESH_TEST_PACKET_ID,
                                                &packet_id);
    entry.packet_id = entry.has_packet_id ? packet_id : 0u;
    ret = app_mesh_ch9_ack_table_queue(&mesh_ch9_ack_table,
                                       ack,
                                       &entry,
                                       &queue_result);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        switch (queue_result) {
        case APP_MESH_CH9_ACK_QUEUE_ADDED:
            status_debug_note("DBG_CH9_ACK_QUEUE_ADD\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_DUPLICATE:
            status_debug_note("DBG_CH9_ACK_QUEUE_DUP\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_REPLACED:
            status_debug_note("DBG_CH9_ACK_QUEUE_REPLACE\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_SUPPRESSED_BY_FORWARDED_ACK:
            status_debug_note("DBG_CH9_ACK_QUEUE_KEEP_GATEWAY\n");
            break;
        case APP_MESH_CH9_ACK_QUEUE_BATCH_FULL:
        case APP_MESH_CH9_ACK_QUEUE_TABLE_FULL:
            status_debug_note("DBG_CH9_ACK_QUEUE_FULL\n");
            break;
        }
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh channel-9 ACK table full: next=0x%016llx seq=%u ret=%d result=%u",
                (unsigned long long)ack->next_hop_id,
                rx->packet.seq,
                ret,
                (unsigned int)queue_result);
        return;
    }
    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
}

static int mesh_ch9_ack_batch_queue_forwarded_gateway_ack(
    const struct mesh_outbound *ack,
    const char *reason)
{
    enum app_mesh_ch9_ack_queue_result queue_result =
        APP_MESH_CH9_ACK_QUEUE_ADDED;
    uint8_t packet_id_count = 0u;
    uint32_t first_packet_id = 0u;
    uint32_t last_packet_id = 0u;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        ack == NULL ||
        ack->packet.msg_type != MSG_GATEWAY_ACK ||
        !mesh_id_is_unicast(ack->next_hop_id)) {
        return -EINVAL;
    }

    ret = app_mesh_ch9_ack_table_queue_forwarded(&mesh_ch9_ack_table,
                                                  ack,
                                                  &queue_result);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh forwarded gateway ACK table full: next=0x%016llx seq=%u ret=%d",
                (unsigned long long)ack->next_hop_id,
                ack->packet.seq,
                ret);
        return mesh_errno_from_proto(ret);
    }
    if (queue_result == APP_MESH_CH9_ACK_QUEUE_REPLACED &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_ACK_QUEUE_REPLACE_GATEWAY\n");
    }

    ret = mesh_ack_payload_packet_id_summary(ack->payload,
                                             ack->payload_len,
                                             &packet_id_count,
                                             &first_packet_id,
                                             &last_packet_id);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_FORWARD_GATEWAY_ACK_QUEUE seq=%u next=0x%llx len=%u ids=%u first=%u last=%u ret=%d reason=%s\n",
                            ack->packet.seq,
                            (unsigned long long)ack->next_hop_id,
                            ack->payload_len,
                            packet_id_count,
                            first_packet_id,
                            last_packet_id,
                            ret,
                            reason == NULL ? "forward-gateway-ack" : reason);
    }
    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    return 0;
}

static int mesh_ch9_ack_batch_build(uint64_t peer_id,
                                    struct mesh_outbound *out)
{
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    int ret;

    if (batch == NULL || out == NULL) {
        return -ENOENT;
    }
    ret = app_mesh_ch9_ack_table_build_peer(&mesh_ch9_ack_table,
                                             peer_id,
                                             out);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_ACK_BUILD peer=0x%llx n=%u forwarded=%u seq=%u len=%u\n",
                            (unsigned long long)peer_id,
                            batch->count,
                            batch->preserve_payload ? 1u : 0u,
                            out->packet.seq,
                            out->payload_len);
    }
    return 0;
}

static int mesh_send_pending_ch9_ack_batch(const struct mesh_event_plan *plan,
                                           uint64_t peer_id,
                                           const char *reason)
{
    struct mesh_outbound ack = {0};
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    uint8_t count;
    int ret;

    if (batch == NULL || batch->count == 0u) {
        return -ENOENT;
    }

    count = batch->count;
    ret = mesh_ch9_ack_batch_build(peer_id, &ack);
    if (ret < 0) {
        return ret;
    }
	    if (plan != NULL) {
	        uint32_t required_ms = 0u;
	        uint32_t now_ms = k_uptime_get_32();

	        if (!mesh_ch9_tx_fits_plan(&ack, plan, now_ms, &required_ms)) {
	            uint32_t retry_delay_ms = 0u;

	            (void)app_mesh_ch9_ack_table_note_send_failure(
	                &mesh_ch9_ack_table,
	                peer_id,
	                now_ms,
	                sys_rand32_get(),
	                &retry_delay_ms);
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                status_debug_note("DBG_CH9_ACK_BATCH_SLOT_FULL\n");
	                status_debug_printf("DBG_CH9_ACK_SLOT_FULL now=%u end=%u req=%u n=%u retry=%u\n",
	                                    now_ms,
	                                    plan->end_ms,
	                                    required_ms,
	                                    count,
	                                    retry_delay_ms);
	            }
            return -EBUSY;
        }
    }

    if (plan != NULL) {
        ack.earliest_tx_ms = mesh_ch9_slot_send_start_ms(&ack,
                                                         plan,
                                                         k_uptime_get_32());
        mesh_ch9_event_set(CH9_EVENT_GRANTED, peer_id, plan, reason);
    }
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        status_debug_note("DBG_CH9_ACK_BATCH_TRY\n");
	    }
    if (plan != NULL) {
        mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD, peer_id, plan, reason);
    }
    ret = mesh_send_outbound(&ack, reason == NULL ? "gateway-ack-batch" : reason);
    if (ret == 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_ACK_BATCH_SENT\n");
        }
        mesh_relay_note_tx_sent(&mesh_runtime, &ack, k_uptime_get_32());
        if (plan != NULL) {
            mesh_note_channel9_local_tx(ack.next_hop_id, plan->start_ms);
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE, peer_id, plan, reason);
        }
        (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer_id);
	    } else {
	        uint32_t retry_delay_ms = 0u;

	        (void)app_mesh_ch9_ack_table_note_send_failure(
	            &mesh_ch9_ack_table,
	            peer_id,
	            k_uptime_get_32(),
	            sys_rand32_get(),
	            &retry_delay_ms);
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_ACK_BATCH_FAIL\n");
	            status_debug_printf("DBG_CH9_ACK_SEND_FAIL ret=%d n=%u retry=%u\n",
	                                ret,
	                                count,
	                                retry_delay_ms);
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER, peer_id, plan, reason);
        }
	    }
    return ret;
}

static int mesh_send_current_ch9_ack_batch(uint64_t peer_id,
                                           const char *reason)
{
    struct mesh_outbound ack = {0};
    const struct app_mesh_ch9_ack_batch *batch =
        app_mesh_ch9_ack_table_get_peer(&mesh_ch9_ack_table, peer_id);
    uint8_t count;
    int ret;

    if (batch == NULL || batch->count == 0u) {
        return -ENOENT;
    }

    count = batch->count;
    ret = mesh_ch9_ack_batch_build(peer_id, &ack);
    if (ret < 0) {
        return ret;
    }

    ack.radio_channel = MESH_EVENT_CHANNEL;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_GATEWAY_BATCH_ACK_GUARD ms=%u n=%u peer=0x%llx reason=%s\n",
                            MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                            count,
                            (unsigned long long)peer_id,
                            reason == NULL ? "batch-ack" : reason);
    }
    k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
    ret = mesh_send_outbound_keep_channel9_awake(
        &ack,
        reason == NULL ? "gateway-batch-ack-current-channel9" : reason,
        NULL);
    if (ret == 0) {
        mesh_relay_note_tx_sent(&mesh_runtime, &ack, k_uptime_get_32());
        mesh_note_channel9_local_tx(ack.next_hop_id, k_uptime_get_32());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_BATCH_ACK_SENT\n");
            status_debug_printf("DBG_GATEWAY_BATCH_ACK_SENT n=%u dst=0x%llx next=0x%llx seq=%u\n",
                                count,
                                (unsigned long long)ack.packet.dst_id,
                                (unsigned long long)ack.next_hop_id,
                                ack.packet.seq);
        }
        (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table, peer_id);
    } else {
        uint32_t retry_delay_ms = 0u;

        /* Keep table custody until the randomized not-before expires. */
        (void)app_mesh_ch9_ack_table_note_send_failure(
            &mesh_ch9_ack_table,
            peer_id,
            k_uptime_get_32(),
            sys_rand32_get(),
            &retry_delay_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_BATCH_ACK_FAIL\n");
            status_debug_printf("DBG_GATEWAY_BATCH_ACK_FAIL ret=%d retry=%u n=%u peer=0x%llx\n",
                                ret,
                                retry_delay_ms,
                                count,
                                (unsigned long long)peer_id);
        }
    }
    return ret;
}

static bool mesh_gateway_batch_ack_retry_wait_ms(uint32_t now_ms,
                                                  uint32_t *wait_ms_out)
{
    uint32_t selected_wait_ms = UINT32_MAX;
    bool found = false;

    if (DEVICE_ROLE != ROLE_GATEWAY || wait_ms_out == NULL) {
        return false;
    }
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        uint32_t wait_ms;

        if (!batch->valid || batch->count == 0u ||
            !batch->retry_deferred || batch->preserve_payload ||
            batch->template_ack.packet.msg_type != MSG_GATEWAY_ACK) {
            continue;
        }
        wait_ms = app_mesh_ch9_ack_table_retry_wait_ms(
            &mesh_ch9_ack_table, batch->peer_id, now_ms);
        if (!found || wait_ms < selected_wait_ms) {
            selected_wait_ms = wait_ms;
            found = true;
        }
    }
    if (found) {
        *wait_ms_out = selected_wait_ms;
    }
    return found;
}

static bool mesh_gateway_handoff_due_batch_acks(uint32_t now_ms)
{
    struct mesh_outbound ack = {0};
    bool admitted_any = false;

    if (DEVICE_ROLE != ROLE_GATEWAY) {
        return false;
    }
    for (uint8_t i = 0u; i < APP_MESH_CH9_ACK_PEER_MAX; i++) {
        const struct app_mesh_ch9_ack_batch *batch =
            &mesh_ch9_ack_table.batches[i];
        uint64_t peer_id;
        uint64_t deadline_ms;
        int ret;

        if (!batch->valid || batch->count == 0u ||
            !batch->retry_deferred || batch->preserve_payload ||
            batch->template_ack.packet.msg_type != MSG_GATEWAY_ACK ||
            !app_mesh_ch9_ack_table_retry_ready(&mesh_ch9_ack_table,
                                                 batch->peer_id,
                                                 now_ms)) {
            continue;
        }

        peer_id = batch->peer_id;
        ret = mesh_ch9_ack_batch_build(peer_id, &ack);
        if (ret == 0) {
            deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;
            ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
            ack.earliest_tx_ms = 0u;
            ret = app_node_comm_submit_control_response(
                &ack, deadline_ms, ack.packet.seq);
        }
        if (ret == 0) {
            (void)app_mesh_ch9_ack_table_clear_peer(&mesh_ch9_ack_table,
                                                     peer_id);
            admitted_any = true;
            status_debug_printf(
                "DBG_GATEWAY_BATCH_ACK_HANDOFF peer=0x%llx seq=%u\n",
                (unsigned long long)peer_id,
                ack.packet.seq);
        } else {
            uint32_t retry_delay_ms = 0u;

            (void)app_mesh_ch9_ack_table_note_send_failure(
                &mesh_ch9_ack_table,
                peer_id,
                now_ms,
                sys_rand32_get(),
                &retry_delay_ms);
            status_debug_printf(
                "DBG_GATEWAY_BATCH_ACK_HANDOFF_WAIT peer=0x%llx ret=%d retry=%u\n",
                (unsigned long long)peer_id,
                ret,
                retry_delay_ms);
        }
    }
    return admitted_any;
}

static void mesh_ch9_tx_pending_clear(void)
{
    memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_ch9_tx_retry_queue_put(const struct mesh_outbound *outbound,
                                       void *ctx)
{
    struct k_msgq *queue = (struct k_msgq *)ctx;
    struct mesh_outbound clean;
    int strip_ret;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }

    clean = *outbound;
    strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && strip_ret != PROTO_OK) {
        status_debug_printf("DBG_CH9_TX_RETRY_STRIP_FAIL ret=%d seq=%u\n",
                            strip_ret,
                            outbound->packet.seq);
    }
    ret = k_msgq_put(queue, strip_ret == PROTO_OK ? &clean : outbound, K_NO_WAIT);
    if (ret == 0) {
        const struct mesh_outbound *retained =
            strip_ret == PROTO_OK ? &clean : outbound;
        uint16_t queue_depth = (uint16_t)k_msgq_num_used_get(queue);

        app_stack_workload_diag_relay_admit(&retained->packet, queue_depth,
                                            (uint16_t)mesh_ch9_tx_pending.count);
        app_stack_workload_diag_relay_sample(&retained->packet, queue_depth,
                                             (uint16_t)mesh_ch9_tx_pending.count);
    }
    return ret;
}

static int mesh_ch9_tx_retry_queue_get(struct mesh_outbound *outbound,
                                       void *ctx)
{
    struct k_msgq *queue = (struct k_msgq *)ctx;

    return k_msgq_get(queue, outbound, K_NO_WAIT);
}

static uint8_t mesh_ch9_tx_retry_queue_used(void *ctx)
{
    struct k_msgq *queue = (struct k_msgq *)ctx;

    return (uint8_t)k_msgq_num_used_get(queue);
}

static void mesh_ch9_tx_retry_note_drop(void *ctx)
{
    ARG_UNUSED(ctx);
    HIGH_DEBUG_COUNTER_INC(mesh_drop);
}
#endif

static uint8_t mesh_ch9_tx_pending_requeue_unacked(uint32_t now_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    struct app_mesh_ch9_tx_retry_entry retry_entries[MESH_CH9_TX_BATCH_MAX];
    const struct app_mesh_ch9_tx_retry_ops ops = {
        .put = mesh_ch9_tx_retry_queue_put,
        .get = mesh_ch9_tx_retry_queue_get,
        .queue_used = mesh_ch9_tx_retry_queue_used,
        .note_drop = mesh_ch9_tx_retry_note_drop,
        .ctx = &report_tx_msgq,
    };
    struct app_mesh_ch9_tx_retry_result result;
    int ret;

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        retry_entries[i].outbound = &mesh_ch9_tx_pending.entries[i].outbound;
        retry_entries[i].acked = &mesh_ch9_tx_pending.entries[i].acked;
    }

    ret = app_mesh_ch9_tx_requeue_unacked(retry_entries,
                                          mesh_ch9_tx_pending.count,
                                          now_ms,
                                          &ops,
                                          &result);
    if (ret != PROTO_OK) {
        memset(&result, 0, sizeof(result));
        result.retained = mesh_ch9_tx_pending.count;
        mesh_ch9_tx_pending.deadline_ms = now_ms + REPORT_TX_RETRY_DELAY_MS;
    } else {
        uint8_t retained_count = 0u;

        for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
            if (!mesh_ch9_tx_pending.entries[i].acked) {
                if (retained_count != i) {
                    mesh_ch9_tx_pending.entries[retained_count] =
                        mesh_ch9_tx_pending.entries[i];
                }
                retained_count++;
            }
        }
        mesh_ch9_tx_pending.count = retained_count;
        mesh_ch9_tx_pending.active = retained_count > 0u;
        if (mesh_ch9_tx_pending.active) {
            mesh_ch9_tx_pending.deadline_ms = now_ms + REPORT_TX_RETRY_DELAY_MS;
        }
    }

    if (result.retained > 0u) {
        uint32_t oldest_age_ms = 0u;

        if (UINT32_MAX - mesh_delivery_health.ack_retry_admission_failures <
            result.retained) {
            mesh_delivery_health.ack_retry_admission_failures = UINT32_MAX;
        } else {
            mesh_delivery_health.ack_retry_admission_failures += result.retained;
        }
        for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
            uint32_t age_ms = now_ms -
                              mesh_ch9_tx_pending.entries[i].outbound.queued_at_ms;

            if (age_ms > oldest_age_ms) {
                oldest_age_ms = age_ms;
            }
        }
        if (oldest_age_ms > mesh_delivery_health.oldest_ack_pending_age_ms) {
            mesh_delivery_health.oldest_ack_pending_age_ms = oldest_age_ms;
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_REQUEUE_PARTIAL requeued=%u retained=%u dropped=%u q_before=%u q_after=%u\n",
                            result.requeued,
                            result.retained,
                            result.dropped,
                            result.queued_before,
                            result.queued_after);
    }
    return result.requeued;
#else
    ARG_UNUSED(now_ms);
    return 0u;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int report_tx_queue_discard_head_locked(void)
{
    return k_msgq_get(&report_tx_msgq, &mesh_send_scratch_tx, K_NO_WAIT);
}

static int report_tx_queue_discard_head(void)
{
    int ret = k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);

    if (ret != 0) {
        return ret;
    }
    ret = report_tx_queue_discard_head_locked();
    k_mutex_unlock(&mesh_send_scratch_lock);
    if (ret == 0) {
        uint16_t queue_depth = (uint16_t)report_tx_queue_used();

        app_stack_workload_diag_click_release(&mesh_send_scratch_tx.packet,
                                              0, queue_depth, 0u);
        app_stack_workload_diag_cir_release(&mesh_send_scratch_tx.packet,
                                            0, queue_depth, 0u);
    }
    return ret;
}
#endif

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_ch9_tx_pending_can_start(void)
{
    return !mesh_ch9_tx_pending.active || mesh_ch9_tx_pending.count == 0u;
}
#endif

static bool mesh_ch9_tx_pending_add(const struct mesh_outbound *sent,
                                    uint32_t deadline_ms)
{
    struct mesh_ch9_tx_pending_entry *entry;
    uint32_t packet_id = 0u;

    if (!app_mesh_ch9_tx_should_track_sent(sent, DEVICE_ID)) {
        return false;
    }

    if (!mesh_ch9_tx_pending.active) {
        memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
        mesh_ch9_tx_pending.active = true;
        mesh_ch9_tx_pending.next_hop_id = sent->next_hop_id;
        mesh_ch9_tx_pending.deadline_ms = deadline_ms;
    }
    if (mesh_ch9_tx_pending.count >= MESH_CH9_TX_BATCH_MAX ||
        mesh_ch9_tx_pending.next_hop_id != sent->next_hop_id) {
        return false;
    }

    entry = &mesh_ch9_tx_pending.entries[mesh_ch9_tx_pending.count++];
    entry->outbound = *sent;
    entry->has_packet_id = mesh_payload_find_u32(sent->payload,
                                                 sent->payload_len,
                                                 TLV_MESH_TEST_PACKET_ID,
                                                 &packet_id);
    entry->packet_id = entry->has_packet_id ? packet_id : 0u;
    entry->acked = false;
    return true;
}

static bool mesh_ch9_tx_pending_track_sent(const struct mesh_outbound *sent,
                                           uint32_t deadline_ms)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !app_mesh_ch9_tx_should_track_sent(sent, DEVICE_ID)) {
        return false;
    }

    if (!app_mesh_ch9_tx_should_track_ack(
            &sent->packet,
            mesh_relay_tx_active_local_collection_result(&mesh_runtime))) {
        return false;
    }

    if (app_mesh_ch9_core_ack_wait_active(&mesh_runtime.pending,
                                          mesh_relay_tx_active(&mesh_runtime))) {
        status_debug_printf("DBG_CH9_TX_ACK_CORE_OWNER src=0x%llx seq=%u next=0x%llx deadline=%u\n",
                            (unsigned long long)sent->packet.src_id,
                            sent->packet.seq,
                            (unsigned long long)sent->next_hop_id,
                            mesh_runtime.pending.gateway_ack_deadline_ms);
        return false;
    }
    if (sent->packet.src_id == DEVICE_ID) {
        LOG_ERR("local gateway-ack TX has no durable relay-core owner: seq=%u next=0x%016llx",
                sent->packet.seq,
                (unsigned long long)sent->next_hop_id);
        return false;
    }

    if (!mesh_ch9_tx_pending_add(sent, deadline_ms)) {
        return false;
    }

    status_debug_printf("DBG_CH9_TX_ACK_TRACKED seq=%u pid=%u n=%u deadline=%u\n",
                        sent->packet.seq,
                        mesh_ch9_tx_pending.entries[mesh_ch9_tx_pending.count - 1u].packet_id,
                        mesh_ch9_tx_pending.count,
                        deadline_ms);
    mesh_relay_cancel_tx(&mesh_runtime);
    return true;
}

static bool mesh_ch9_tx_pending_handle_ack(const struct proto_packet *packet,
                                           const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t previous_hop_id)
{
    struct app_mesh_ch9_tx_ack_entry ack_entries[MESH_CH9_TX_BATCH_MAX];
    struct app_mesh_ch9_tx_ack_result ack_result;
    uint64_t ack_peer_id = 0u;
    bool dst_matches_pending_source = false;
    int ret;

    if (packet == NULL) {
        return false;
    }
    if (packet->msg_type != MSG_GATEWAY_ACK) {
        return false;
    }
    if (!mesh_ch9_tx_pending.active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_IDLE\n");
        }
        return false;
    }
    if (payload == NULL) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_NULL_PAYLOAD\n");
        }
        return false;
    }
    if (previous_hop_id != mesh_ch9_tx_pending.next_hop_id ||
        packet->src_id != mesh_ch9_tx_pending.next_hop_id) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_ACK_PEER_MISMATCH src=0x%llx prev=0x%llx expect=0x%llx\n",
                                (unsigned long long)packet->src_id,
                                (unsigned long long)previous_hop_id,
                                (unsigned long long)mesh_ch9_tx_pending.next_hop_id);
        }
        return false;
    }
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        if (packet->dst_id == mesh_ch9_tx_pending.entries[i].outbound.packet.src_id) {
            dst_matches_pending_source = true;
            break;
        }
    }
    if (packet->dst_id != DEVICE_ID &&
        !(packet->msg_type == MSG_GATEWAY_ACK &&
          previous_hop_id == mesh_ch9_tx_pending.next_hop_id &&
          dst_matches_pending_source)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_DST_MISMATCH\n");
            status_debug_printf("DBG_CH9_TX_ACK_DST dst=0x%llx expect=0x%llx pending_src=%u prev=0x%llx next=0x%llx\n",
                                (unsigned long long)packet->dst_id,
                                (unsigned long long)DEVICE_ID,
                                dst_matches_pending_source ? 1u : 0u,
                                (unsigned long long)previous_hop_id,
                                (unsigned long long)mesh_ch9_tx_pending.next_hop_id);
        }
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint8_t id_count = 0u;
        uint32_t first_id = 0u;
        uint32_t last_id = 0u;
        int id_ret;

        status_debug_note("DBG_CH9_TX_ACK_CANDIDATE\n");
        id_ret = mesh_ack_payload_packet_id_summary(payload,
                                                    payload_len,
                                                    &id_count,
                                                    &first_id,
                                                    &last_id);
        if (id_ret == PROTO_OK) {
            status_debug_printf("DBG_CH9_TX_ACK_PACKET_IDS n=%u first=%u last=%u\n",
                                id_count,
                                first_id,
                                last_id);
        } else {
            status_debug_printf("DBG_CH9_TX_ACK_PACKET_IDS_BAD ret=%d\n", id_ret);
        }
    }

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        ack_entries[i].session_id =
            mesh_ch9_tx_pending.entries[i].outbound.packet.session_id;
        ack_entries[i].seq = mesh_ch9_tx_pending.entries[i].outbound.packet.seq;
        ack_entries[i].acked = mesh_ch9_tx_pending.entries[i].acked;
    }
    ret = app_mesh_ch9_tx_ack_apply(packet,
                                    payload,
                                    payload_len,
                                    ack_entries,
                                    mesh_ch9_tx_pending.count,
                                    &ack_result);
    if (ret != PROTO_OK) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_ACK_MALFORMED\n");
            status_debug_printf("DBG_CH9_TX_ACK_BAD_PAYLOAD ret=%d seq=%u\n",
                                ret,
                                mesh_ch9_tx_pending.count == 0u ? 0u :
                                mesh_ch9_tx_pending.entries[0].outbound.packet.seq);
        }
        return false;
    }
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        mesh_ch9_tx_pending.entries[i].acked = ack_entries[i].acked;
    }

	    if (!ack_result.any_match) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_TX_ACK_NO_MATCH\n");
	            status_debug_printf("DBG_CH9_TX_ACK_NO_MATCH n=%u first=%u ackseq=%u\n",
	                                mesh_ch9_tx_pending.count,
	                                mesh_ch9_tx_pending.count == 0u ? 0u :
	                                mesh_ch9_tx_pending.entries[0].outbound.packet.seq,
	                                packet->seq);
	        }
	        return false;
	    }

    if (!ack_result.all_acked) {
            uint8_t pending_count = mesh_ch9_tx_pending.count;
            uint8_t requeued;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_ACK_PARTIAL\n");
                status_debug_printf("DBG_CH9_TX_ACK_PARTIAL acked=%u n=%u\n",
                                    ack_result.acked_now,
                                    pending_count);
            }
            requeued = mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
            if (!mesh_ch9_tx_pending.active) {
                mesh_ch9_tx_pending_clear();
            }
            mesh_schedule_tx_timeout();
            if (requeued > 0u || report_tx_queue_used() > 0u) {
                report_tx_schedule(0u);
            }
            return true;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_COMPLETE acked=%u n=%u\n",
                            ack_result.acked_now,
                            mesh_ch9_tx_pending.count);
        if (report_tx_queue_used() > 0u) {
            status_debug_printf("DBG_CH9_TX_ACK_QUEUE_PARTIAL acked=%u queued=%u\n",
                                ack_result.acked_now,
                                report_tx_queue_used());
        }
    }
    ack_peer_id = mesh_ch9_tx_pending.next_hop_id;
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        app_stack_workload_diag_relay_release(
            &mesh_ch9_tx_pending.entries[i].outbound.packet, 0,
            (uint16_t)report_tx_queue_used(), 0u);
    }
    mesh_ch9_tx_pending_clear();
    app_mesh_persistence_clear_outbox();
    mesh_schedule_tx_timeout();
    report_tx_schedule(0u);
    const struct app_mesh_ch9_ack_complete_state complete_state = {
        .route_test_enabled = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST),
        .transmitter_role = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
        .report_tx_queue_used = report_tx_queue_used(),
        .route_waiting_tx_valid = mesh_route_waiting_tx_valid,
        .ack_batch_valid =
            app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table),
    };

    if (app_mesh_ch9_ack_complete_should_close_timing(&complete_state)) {
        mesh_close_channel9_connection(ack_peer_id, "ch9-idle-ack-complete");
    }
    return true;
}

static uint16_t mesh_route_wake_claimed_duration_ms(
    uint16_t wake_train_ends_in_ms,
    const struct app_clicker_wake_train_config *config)
{
    uint32_t claimed_ms;

    if (config == NULL) {
        return wake_train_ends_in_ms;
    }

    claimed_ms = (uint32_t)wake_train_ends_in_ms +
                 config->post_wake_claimed_duration_ms;
    return claimed_ms > UINT16_MAX ? UINT16_MAX : (uint16_t)claimed_ms;
}

static int mesh_route_wake_sniff_activity(const char *phase,
                                          uint64_t target_id,
                                          const char *reason,
                                          uint8_t retry_index,
                                          bool *activity)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    int ret;

    if (activity == NULL) {
        return -EINVAL;
    }
    *activity = false;

    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        return ret;
    }
    ret = dwm3000_driver_sniff_activity(APP_WAKE_TRAIN_POLITE_SNIFF_MS,
                                        &rx_failure);
    (void)dwm3000_driver_standby();

    *activity = app_wake_train_politeness_rx_activity(ret, rx_failure);
    if (*activity) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_WAKE_POLITE_ACTIVITY phase=%s target=0x%llx retry=%u ret=%d fail=%u reason=%s\n",
                                phase == NULL ? "unknown" : phase,
                                (unsigned long long)target_id,
                                retry_index,
                                ret,
                                (unsigned int)rx_failure,
                                reason == NULL ? "route" : reason);
        }
        LOG_INF("mesh route wake train C5 activity during %s sniff: target=0x%016llx retry=%u ret=%d failure=%u reason=%s",
                phase == NULL ? "unknown" : phase,
                (unsigned long long)target_id,
                retry_index,
                ret,
                (unsigned int)rx_failure,
                reason == NULL ? "route" : reason);
        return 0;
    }

    return ret == -ETIMEDOUT ? 0 : ret;
}

static void mesh_route_wake_backoff(uint64_t target_id,
                                    const char *phase,
                                    const char *reason,
                                    uint8_t retry_index)
{
    uint32_t delay_ms = app_wake_train_politeness_backoff_ms(
        retry_index,
        sys_rand32_get());

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_WAKE_POLITE_BACKOFF phase=%s target=0x%llx retry=%u/%u delay=%u reason=%s\n",
                            phase == NULL ? "unknown" : phase,
                            (unsigned long long)target_id,
                            (uint32_t)retry_index + 1u,
                            APP_WAKE_TRAIN_POLITE_MAX_RETRIES,
                            delay_ms,
                            reason == NULL ? "route" : reason);
    }
    LOG_INF("mesh route wake train deferred after C5 activity: target=0x%016llx retry=%u/%u phase=%s backoff_ms=%u reason=%s",
            (unsigned long long)target_id,
            (uint32_t)retry_index + 1u,
            APP_WAKE_TRAIN_POLITE_MAX_RETRIES,
            phase == NULL ? "unknown" : phase,
            delay_ms,
            reason == NULL ? "route" : reason);
    k_msleep(delay_ms);
}

static int mesh_route_wake_listen_for_click(
    uint8_t *frame,
    struct uwb_wake_claim_frame *click_claim,
    uint8_t *click_quality,
    uint32_t *click_observed_ms,
    bool *activity)
{
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    int ret;

    if (frame == NULL || click_claim == NULL || click_quality == NULL ||
        click_observed_ms == NULL || activity == NULL) {
        return -EINVAL;
    }

    *activity = false;
    ret = dwm3000_driver_receive_frame_continuous_extend_on_activity(
        ANCHOR_UWB_SCAN_RX_MS,
        ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS,
        frame,
        MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
        &frame_len,
        &quality,
        NULL,
        &rx_failure);
    *activity = app_wake_train_politeness_rx_activity(ret, rx_failure);
    if (ret == 0 &&
        mesh_frame_requires_anchor_click_handoff(frame,
                                                 frame_len,
                                                 click_claim)) {
        *click_quality = quality;
        *click_observed_ms = k_uptime_get_32();
        status_debug_printf("DBG_ROUTE_WAKE_CLICK_CAPTURE evt=%u attempt=%u q=%u\n",
                            click_claim->click_event_id,
                            click_claim->attempt_index,
                            quality);
        return 1;
    }

    if (*activity) {
        status_debug_printf("DBG_ROUTE_WAKE_RX_ACTIVITY ret=%d fail=%u len=%u click=0\n",
                            ret,
                            (unsigned int)rx_failure,
                            (unsigned int)frame_len);
        return 0;
    }

    return ret == -ETIMEDOUT ? 0 : ret;
}

static int mesh_send_route_wake_train(uint64_t target_id,
                                      const struct mesh_outbound *embedded_route_req,
                                      bool *embedded_sent,
                                      uint8_t purpose,
                                      const char *reason)
{
    struct uwb_clicker_session *session = &mesh_route_wake_session_scratch;
    struct uwb_clicker_config *config = &mesh_route_wake_config_scratch;
    const struct app_clicker_wake_train_config wake_train_config = {
        .wake_adv_ms = WAKE_ADV_MS,
        .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
        .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    };
    uint8_t *route_suffix = mesh_route_wake_suffix_scratch;
    uint8_t *frame = mesh_route_wake_frame_scratch;
    size_t route_suffix_len = 0u;
    bool embed_route = false;
    uint32_t route_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    uint32_t event_seq;
    uint32_t next_click_rx_ms = 0u;
    uint32_t click_observed_ms = 0u;
    int64_t close_ms;
    uint16_t sent_count = 0u;
    uint16_t embedded_count = 0u;
    uint8_t polite_retry = 0u;
    uint8_t click_quality = 0u;
    bool c5_activity = false;
    bool click_captured = false;
    bool local_can_range_clicks =
        DEVICE_ROLE == ROLE_ANCHOR &&
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);
    struct uwb_wake_claim_frame click_claim = {0};
    const char *activity_phase = NULL;
    int ret;

    if (embedded_sent != NULL) {
        *embedded_sent = false;
    }
    if (target_id != MESH_BROADCAST_ID &&
        (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID)) {
        return -EINVAL;
    }
    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-wake-train" : reason)) {
        return -EBUSY;
    }

    ret = k_mutex_lock(&mesh_route_wake_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    event_seq = nonzero_uptime_session_id();
    memset(config, 0, sizeof(*config));
    config->network_id = NETWORK_ID;
    config->clicker_id = DEVICE_ID;
    config->click_event_id = event_seq;
    config->nonce = clicker_nonce(event_seq);
    config->min_anchor_count = 1u;
    config->max_anchor_count = 1u;
    config->max_attempts = 1u;
    config->samples_per_anchor = 1u;
    config->wake_channel = UWB_CHANNEL_WAKE_CONTACT;
    config->ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
    config->flags = FLAG_ROUTE_SETUP | FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;
    if (purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD) {
        config->flags |= FLAG_CONTROL_FOLLOWUP;
    }

    ret = uwb_clicker_session_start(session, config);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh channel-5 wake-train session rejected: target=0x%016llx ret=%d reason=%s",
                (unsigned long long)target_id,
                ret,
                reason == NULL ? "route" : reason);
        ret = -EINVAL;
        goto out_unlock;
    }

    if (embedded_route_req != NULL && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        route_reply_window_ms =
            mesh_route_reply_listen_window_ms(embedded_route_req->packet.ttl);
        ret = mesh_route_wake_encode_suffix(embedded_route_req,
                                            route_suffix,
                                            MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN,
                                            &route_suffix_len);
        if (ret == 0) {
            embed_route = true;
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_SUFFIX payload=%u suffix=%u cap=%u\n",
                                embedded_route_req->payload_len,
                                (unsigned int)route_suffix_len,
                                (unsigned int)MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN);
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_WINDOW ttl=%u win=%u\n",
                                embedded_route_req->packet.ttl,
                                route_reply_window_ms);
        } else {
            status_debug_printf("DBG_EMBEDDED_ROUTE_REQ_DISABLED ret=%d payload=%u cap=%u\n",
                                ret,
                                embedded_route_req->payload_len,
                                (unsigned int)MESH_ROUTE_WAKE_ROUTE_SUFFIX_MAX_LEN);
            LOG_WRN("mesh channel-5 wake-train embedded request disabled: target=0x%016llx ret=%d reason=%s",
                    (unsigned long long)target_id,
                    ret,
                    reason == NULL ? "route" : reason);
        }
    }

wake_train_attempt:
    sent_count = 0u;
    embedded_count = 0u;
    c5_activity = false;
    activity_phase = NULL;

    mesh_stop_role_scan();
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=start target=0x%016llx event_seq=%u embed=%u retry=%u reason=%s",
                         (unsigned long long)target_id,
                         event_seq,
                         embed_route ? 1u : 0u,
                         polite_retry,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train start: target=0x%016llx event_seq=%u embed=%u retry=%u reason=%s",
            (unsigned long long)target_id,
            event_seq,
            embed_route ? 1u : 0u,
            polite_retry,
            reason == NULL ? "route" : reason);
    mesh_c5_contact_open(target_id,
                         purpose,
                         event_seq,
                         true,
                         k_uptime_get_32() + wake_train_config.wake_adv_ms +
                         wake_train_config.post_wake_claimed_duration_ms,
                         reason);

    ret = mesh_transport_radio_start("mesh route WAKE_CLAIM train");
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_GUARD_FAIL\n");
        LOG_WRN("mesh route UWB WAKE_CLAIM guard failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        mesh_c5_contact_clear("wake-train-guard-fail");
        mesh_restart_role_scan();
        goto out_unlock;
    }
    status_debug_note("DBG_WAKE_TRAIN_GUARD_OK\n");

    ret = mesh_route_wake_sniff_activity("pre",
                                         target_id,
                                         reason,
                                         polite_retry,
                                         &c5_activity);
    if (ret < 0) {
        goto out;
    }
    if (c5_activity) {
        activity_phase = "pre";
        ret = -EAGAIN;
        goto out;
    }

    stage1_led_phase(STAGE1_LED_PHASE_WAKE);
    stage1_led_result(STAGE1_LED_RESULT_ACTIVE);

    status_debug_note("DBG_WAKE_TRAIN_CONFIG_BEGIN\n");
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        status_debug_note("DBG_WAKE_TRAIN_CONFIG_FAIL\n");
        LOG_WRN("mesh route UWB WAKE_CLAIM wake-mode config failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        goto out;
    }
    status_debug_note("DBG_WAKE_TRAIN_CONFIG_OK\n");

    close_ms = k_uptime_get() + wake_train_config.wake_adv_ms;
    next_click_rx_ms = k_uptime_get_32() + MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS;
    while (k_uptime_get() < close_ms) {
        struct uwb_wake_claim_frame claim;
        size_t frame_len = 0u;
        int64_t remaining_ms = close_ms - k_uptime_get();
        uint16_t remaining_u16 = delay_ms_to_u16(remaining_ms);

        ret = uwb_clicker_build_wake_claim(session,
                                           clicker_priority_id(event_seq,
                                                               session->attempt_index),
                                           remaining_u16,
                                           remaining_u16,
                                           mesh_route_wake_claimed_duration_ms(
                                               remaining_u16,
                                               &wake_train_config),
                                           &claim);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_BUILD_FAIL\n");
            LOG_WRN("mesh route UWB WAKE_CLAIM build failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }

        ret = uwb_encode_wake_claim(&claim,
                                    frame,
                                    MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
                                    &frame_len);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_ENCODE_FAIL\n");
            LOG_WRN("mesh route UWB WAKE_CLAIM encode failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }

        if (embed_route &&
            frame_len + route_suffix_len <= MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN &&
            k_uptime_get() < close_ms) {
            memcpy(&frame[frame_len], route_suffix, route_suffix_len);
            frame_len += route_suffix_len;
            ret = dwm3000_driver_send_frame(frame,
                                            frame_len,
                                            wake_train_config.control_tx_timeout_ms);
            if (ret < 0) {
                status_debug_note("DBG_EMBEDDED_ROUTE_REQ_TX_FAIL\n");
                LOG_WRN("mesh route embedded WAKE_CLAIM send failed: target=0x%016llx sent=%u embedded=%u ret=%d frame_len=%u",
                        (unsigned long long)target_id,
                        sent_count,
                        embedded_count,
                        ret,
                        (unsigned int)frame_len);
                break;
            }
            status_debug_note("DBG_EMBEDDED_ROUTE_REQ_TX\n");
            status_debug_tx_wake_claim_sent_pulse();
            sent_count++;
            embedded_count++;
            HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
            uwb_clicker_note_wake_claim_tx(session, 1u);
        }

        if (k_uptime_get() >= close_ms) {
            continue;
        }

        ret = uwb_encode_wake_claim(&claim,
                                    frame,
                                    MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN,
                                    &frame_len);
        if (ret != PROTO_OK) {
            status_debug_note("DBG_WAKE_TRAIN_ENCODE_FAIL\n");
            LOG_WRN("mesh route plain WAKE_CLAIM encode failed: target=0x%016llx proto_ret=%d",
                    (unsigned long long)target_id,
                    ret);
            ret = -EINVAL;
            break;
        }
        if (sent_count == 0u) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_BEGIN\n");
        }
        ret = dwm3000_driver_send_frame(frame,
                                        frame_len,
                                        wake_train_config.control_tx_timeout_ms);
        if (ret < 0) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_FAIL\n");
            LOG_WRN("mesh route plain WAKE_CLAIM send failed: target=0x%016llx sent=%u ret=%d frame_len=%u",
                    (unsigned long long)target_id,
                    sent_count,
                    ret,
                    (unsigned int)frame_len);
            break;
        }
        if (sent_count == 0u) {
            status_debug_note("DBG_WAKE_TRAIN_FIRST_SEND_OK\n");
        }
        status_debug_tx_wake_claim_sent_pulse();
        sent_count++;
        HIGH_DEBUG_COUNTER_INC(wake_claim_tx);
        uwb_clicker_note_wake_claim_tx(session, 1u);

        if (local_can_range_clicks &&
            uptime_deadline_reached(k_uptime_get_32(), next_click_rx_ms)) {
            bool rx_activity = false;
            int listen_ret;

            next_click_rx_ms = k_uptime_get_32() +
                               MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS;
            listen_ret = mesh_route_wake_listen_for_click(frame,
                                                          &click_claim,
                                                          &click_quality,
                                                          &click_observed_ms,
                                                          &rx_activity);
            if (listen_ret > 0) {
                click_captured = true;
                ret = -ECANCELED;
                goto out;
            }
            if (rx_activity) {
                c5_activity = true;
                activity_phase = "train";
                ret = -EAGAIN;
                goto out;
            }
            if (listen_ret < 0) {
                status_debug_printf("DBG_ROUTE_WAKE_CLICK_RX_FAIL ret=%d sent=%u\n",
                                    listen_ret,
                                    sent_count);
                ret = listen_ret;
                goto out;
            }
            ret = 0;
        }

        if (k_uptime_get() < close_ms) {
            uint32_t jitter_us = uwb_clicker_wake_claim_jitter_us(sys_rand32_get());
            int64_t remaining_after_tx_ms = close_ms - k_uptime_get();

            if (jitter_us > 0u && remaining_after_tx_ms > 0) {
                k_busy_wait(jitter_us);
            }
        }
    }

    if (ret >= 0 && sent_count > 0u) {
        ret = mesh_route_wake_sniff_activity("post",
                                             target_id,
                                             reason,
                                             polite_retry,
                                             &c5_activity);
        if (ret < 0) {
            goto out;
        }
        if (c5_activity) {
            activity_phase = "post";
            ret = -EAGAIN;
            goto out;
        }
    }

out:
    (void)dwm3000_driver_standby();
    radio_guard_uwb_stop();

    if (click_captured) {
        mesh_c5_contact_clear("click-preempt");
        stage1_led_result(STAGE1_LED_RESULT_ACTIVE);
        high_debug_log_event("MESH_CH5_WAKE_TX",
                             "phase=click-preempt target=0x%016llx event_seq=%u click_event=%u attempt=%u wake_claims=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             click_claim.click_event_id,
                             click_claim.attempt_index,
                             sent_count,
                             reason == NULL ? "route" : reason);
        goto out_unlock;
    }

    if (ret < 0) {
        stage1_led_result(STAGE1_LED_RESULT_ERROR);
        mesh_c5_contact_clear("wake-train-fail");
        if (ret == -EAGAIN && c5_activity &&
            polite_retry < APP_WAKE_TRAIN_POLITE_MAX_RETRIES) {
            mesh_restart_role_scan();
            mesh_route_wake_backoff(target_id,
                                    activity_phase,
                                    reason,
                                    polite_retry);
            polite_retry++;
            goto wake_train_attempt;
        }
        high_debug_log_event("MESH_CH5_WAKE_TX",
                             "phase=fail target=0x%016llx event_seq=%u ret=%d wake_claims=%u embedded=%u retry=%u activity=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             ret,
                             sent_count,
                             embedded_count,
                             polite_retry,
                             c5_activity ? 1u : 0u,
                             reason == NULL ? "route" : reason);
        LOG_WRN("mesh route channel-5 wake train failed: target=0x%016llx ret=%d wake_claims=%u embedded=%u retry=%u activity=%u phase=%s reason=%s",
                (unsigned long long)target_id,
                ret,
                sent_count,
                embedded_count,
                polite_retry,
                c5_activity ? 1u : 0u,
                activity_phase == NULL ? "none" : activity_phase,
                reason == NULL ? "route" : reason);
        goto out_unlock;
    }
    if (embedded_sent != NULL) {
        *embedded_sent = embedded_count > 0u;
    }
    if (sent_count > 0u) {
        mesh_c5_contact_accept(target_id,
                               C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                               k_uptime_get_32() +
                               wake_train_config.post_wake_claimed_duration_ms +
                               route_reply_window_ms,
                               reason);
    } else {
        mesh_c5_contact_clear("wake-train-timeout");
    }
    stage1_led_result(sent_count == 0u ?
                      STAGE1_LED_RESULT_TIMEOUT :
                      STAGE1_LED_RESULT_OK);
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=done target=0x%016llx event_seq=%u ret=%d wake_claims=%u embedded=%u reason=%s",
                             (unsigned long long)target_id,
                             event_seq,
                             sent_count == 0u ? -ETIMEDOUT : 0,
                             session->diagnostics.wake_claim_tx_count,
                             embedded_count,
                             reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train complete: target=0x%016llx ret=%d wake_claims=%u embedded=%u reason=%s",
            (unsigned long long)target_id,
            sent_count == 0u ? -ETIMEDOUT : 0,
            sent_count,
            embedded_count,
            reason == NULL ? "route" : reason);
    ret = sent_count == 0u ? -ETIMEDOUT : 0;

out_unlock:
    k_mutex_unlock(&mesh_route_wake_scratch_lock);
    if (click_captured) {
        bool handled = mesh_handoff_anchor_click_claim(&click_claim,
                                                       click_quality,
                                                       click_observed_ms);

        return handled ? -ECANCELED : -EIO;
    }
    return ret;
}

static uint8_t mesh_c5_listener_purpose(const char *reason)
{
    if (reason != NULL && strcmp(reason, "event-accept") == 0) {
        return C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION;
    }
    if (reason != NULL && strcmp(reason, "gateway-command-wake-followup") == 0) {
        return C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD;
    }
    return C5_CONTACT_PURPOSE_ROUTE_SOLICIT;
}

static bool mesh_route_capture_find_u32(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint32_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (value == NULL ||
        tlv_find(payload, payload_len, type,
                 &encoded, &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(*value)) {
        return false;
    }

    *value = proto_get_u32_le(encoded);
    return true;
}

static bool mesh_route_capture_find_u16(const uint8_t *payload,
                                        size_t payload_len,
                                        uint8_t type,
                                        uint16_t *value)
{
    const uint8_t *encoded = NULL;
    uint8_t encoded_len = 0u;

    if (value == NULL ||
        tlv_find(payload, payload_len, type,
                 &encoded, &encoded_len) != PROTO_OK ||
        encoded_len != sizeof(*value)) {
        return false;
    }

    *value = proto_get_u16_le(encoded);
    return true;
}

static bool mesh_route_capture_identity_from_request(
    const struct mesh_outbound *request,
    uint64_t target_id,
    struct mesh_route_capture_identity *identity)
{
    uint32_t flood_epoch_id = 0u;

    if (request == NULL || identity == NULL ||
        request->packet.msg_type != MSG_ROUTE_REQ ||
        request->packet.session_id == 0u ||
        !mesh_id_is_unicast(target_id) ||
        !mesh_route_capture_find_u32(request->payload,
                                     request->payload_len,
                                     TLV_FLOOD_EPOCH_ID,
                                     &flood_epoch_id) ||
        flood_epoch_id == 0u) {
        return false;
    }

    *identity = (struct mesh_route_capture_identity) {
        .session_id = request->packet.session_id,
        .flood_epoch_id = flood_epoch_id,
        .reply_nonce = mesh_route_reply_nonce(request->packet.src_id,
                                              target_id,
                                              request->packet.session_id,
                                              flood_epoch_id),
    };
    return identity->reply_nonce != 0u;
}

static int mesh_listen_for_route_reply(uint64_t target_id,
                                       const char *reason,
                                       uint32_t window_ms,
                                       const struct mesh_route_capture_identity *identity,
                                       bool *route_reply_captured)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    size_t capture_count = 0u;
    bool captured_route_reply = false;
    bool process_event_control_after_rx = false;
    bool rx_failure_diagnostic_logged = false;
    int64_t uwb_window_start_ms = -1;
    uint32_t deadline_ms;
    uint8_t contact_purpose;
    int last_ret = -ETIMEDOUT;
    int ret;

    if (route_reply_captured != NULL) {
        *route_reply_captured = false;
    }

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY) {
        return 0;
    }
    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }
    if (window_ms == 0u) {
        window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-reply-listen" : reason)) {
        return -EBUSY;
    }

    contact_purpose = mesh_c5_listener_purpose(reason);
    mesh_c5_contact_exchange(target_id,
                             contact_purpose,
                             k_uptime_get_32() + window_ms,
                             reason);

    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh route reply RX");
    if (ret < 0) {
        mesh_c5_contact_clear("reply-listen-guard-fail");
        mesh_restart_role_scan();
        return ret;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=start target=0x%016llx window_ms=%u reason=%s",
                         (unsigned long long)target_id,
                         window_ms,
                         reason == NULL ? "route" : reason);
    status_debug_note("DBG_ROUTE_REPLY_LISTEN_START\n");
    LOG_INF("mesh route reply listen start: target=0x%016llx window_ms=%u reason=%s",
            (unsigned long long)target_id,
            window_ms,
            reason == NULL ? "route" : reason);

    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mesh_control_mode();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_LISTEN_CONFIG ret=%d ext=1\n", ret);
    }
    if (ret == 0) {
        deadline_ms = k_uptime_get_32() + window_ms;
        while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms) &&
               capture_count < MESH_ROUTE_TEST_REPLY_CAPTURE_MAX) {
            struct mesh_frame_parse_context parsed = {0};
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint32_t captured_flood_epoch_id = 0u;
            uint16_t captured_reply_nonce = 0u;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
            size_t frame_len = 0u;
            uint8_t quality = 0u;

            if (remaining_ms == 0u) {
                break;
            }

            ret = dwm3000_driver_receive_frame_continuous(remaining_ms,
                                                          frame,
                                                          UWB_MESH_MAX_FRAME_LEN,
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
            last_ret = ret;
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                if (!rx_failure_diagnostic_logged &&
                    IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    struct dwm3000_rx_debug_snapshot snapshot = {0};

                    dwm3000_driver_last_rx_debug_get(&snapshot);
                    status_debug_printf(
                        "DBG_C5_RX_FAIL status=0x%08x finfo=0x%08x fail=%u phy=%u ch=%u plen=%u pac=%u txc=%u rxc=%u sfd=%u rate=%u phr=%u phrr=%u sfdto=%u\n",
                        snapshot.status,
                        snapshot.rx_finfo,
                        (unsigned int)rx_failure,
                        snapshot.phy_mode,
                        snapshot.channel,
                        snapshot.preamble_length,
                        snapshot.pac,
                        snapshot.tx_code,
                        snapshot.rx_code,
                        snapshot.sfd_type,
                        snapshot.data_rate,
                        snapshot.phr_mode,
                        snapshot.phr_rate,
                        snapshot.sfd_timeout);
                    rx_failure_diagnostic_logged = true;
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=rx-fail target=0x%016llx ret=%d rx_failure=%u reason=%s",
                                     (unsigned long long)target_id,
                                     ret,
                                     (unsigned int)rx_failure,
                                     reason == NULL ? "route" : reason);
                LOG_WRN("mesh route reply RX failed: target=0x%016llx ret=%d rx_failure=%u reason=%s",
                        (unsigned long long)target_id,
                        ret,
                        (unsigned int)rx_failure,
                        reason == NULL ? "route" : reason);
                continue;
            }

            status_debug_note("DBG_ROUTE_REPLY_RX_FRAME\n");
            {
                bool embedded_route_frame = false;
                bool click_priority = false;

                if (mesh_handle_channel5_wake_claim(frame,
                                                    frame_len,
                                                    quality,
                                                    &embedded_route_frame,
                                                    &click_priority)) {
                    uint32_t preempted_at_ms = k_uptime_get_32();

                    if (DEVICE_ROLE == ROLE_ANCHOR && click_priority) {
                        last_ret = -EAGAIN;
                        status_debug_note("DBG_ROUTE_REPLY_CLICK_PREEMPT\n");
                        high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                             "phase=click-preempt target=0x%016llx reason=%s",
                                             (unsigned long long)target_id,
                                             reason == NULL ? "route" : reason);
                        break;
                    }
                    deadline_ms = app_mesh_route_reply_ack_deadline_after_preemption(
                        preempted_at_ms,
                        window_ms,
                        deadline_ms);
                    status_debug_note("DBG_ROUTE_REPLY_RX_PREEMPT_BOUNDED\n");
                    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                         "phase=c5-preempt target=0x%016llx cumulative_deadline=%u reason=%s",
                                         (unsigned long long)target_id,
                                         deadline_ms,
                                         reason == NULL ? "route" : reason);
                    continue;
                }
            }
            ret = uwb_mesh_frame_decode(frame,
                                        frame_len,
                                        NETWORK_ID,
                                        DEVICE_ID,
                                        &parsed.previous_hop_id,
                                        &parsed.packet,
                                        parsed.payload,
                                        sizeof(parsed.payload),
                                        &parsed.payload_len);
            if (ret != PROTO_OK || parsed.payload_len > UINT8_MAX) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_DECODE_FAIL ret=%d len=%u q=%u\n",
                                        ret,
                                        (unsigned int)frame_len,
                                        quality);
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=reject target=0x%016llx len=%u quality=%u decode_ret=%d reason=%s",
                                     (unsigned long long)target_id,
                                     (unsigned int)frame_len,
                                     quality,
                                     ret,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen ignored non-mesh frame: target=0x%016llx len=%u quality=%u ret=%d",
                        (unsigned long long)target_id,
                        (unsigned int)frame_len,
                        quality,
                        ret);
                continue;
            }

            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                status_debug_note("DBG_ROUTE_REPLY_RX\n");
            } else if (parsed.packet.msg_type == MSG_GATEWAY_ROUTE_ADV) {
                status_debug_note("DBG_GATEWAY_ROUTE_ADV_RX\n");
            } else if (parsed.packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_ACCEPT ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_UPDATE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_END) {
                status_debug_note("DBG_EVENT_CTRL_RX\n");
            }

            if (app_mesh_rx_policy_should_drop(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
                                               parsed.packet.msg_type)) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_POLICY_DROP msg=0x%02x prev=0x%llx seq=%u\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.previous_hop_id,
                                        parsed.packet.seq);
                }
                continue;
            }

            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                (void)mesh_route_capture_find_u32(parsed.payload,
                                                  parsed.payload_len,
                                                  TLV_FLOOD_EPOCH_ID,
                                                  &captured_flood_epoch_id);
                (void)mesh_route_capture_find_u16(parsed.payload,
                                                  parsed.payload_len,
                                                  TLV_REPLY_NONCE,
                                                  &captured_reply_nonce);
            }

            if (!app_mesh_c5_route_capture_relevant(
                    &(const struct app_mesh_c5_route_capture_state) {
                        .msg_type = parsed.packet.msg_type,
                        .session_id = parsed.packet.session_id,
                        .flood_epoch_id = captured_flood_epoch_id,
                        .reply_nonce = captured_reply_nonce,
                        .src_id = parsed.packet.src_id,
                        .dst_id = parsed.packet.dst_id,
                        .previous_hop_id = parsed.previous_hop_id,
                        .target_id = target_id,
                        .local_id = DEVICE_ID,
                        .control_origin_id = GATEWAY_ID,
                        .expected_session_id =
                            identity == NULL ? 0u : identity->session_id,
                        .expected_flood_epoch_id =
                            identity == NULL ? 0u : identity->flood_epoch_id,
                        .expected_reply_nonce =
                            identity == NULL ? 0u : identity->reply_nonce,
                        .route_identity_required = identity != NULL,
                        .control_followup =
                            contact_purpose ==
                                C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD,
                    })) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_ROUTE_REPLY_UNRELATED msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx target=0x%llx session=%u/%u flood=%u/%u nonce=%u/%u\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.packet.src_id,
                                        (unsigned long long)parsed.packet.dst_id,
                                        (unsigned long long)parsed.previous_hop_id,
                                        (unsigned long long)target_id,
                                        parsed.packet.session_id,
                                        identity == NULL ? 0u : identity->session_id,
                                        captured_flood_epoch_id,
                                        identity == NULL ? 0u : identity->flood_epoch_id,
                                        captured_reply_nonce,
                                        identity == NULL ? 0u : identity->reply_nonce);
                }
                LOG_INF("mesh route reply listen ignored unrelated mesh frame: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                        parsed.packet.msg_type,
                        (unsigned long long)parsed.packet.src_id,
                        (unsigned long long)parsed.packet.dst_id,
                        (unsigned long long)parsed.previous_hop_id,
                        parsed.packet.seq,
                        quality);
                continue;
            }

            if (DEVICE_ROLE == ROLE_ANCHOR &&
                contact_purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD) {
                enum command_id control_command_id = CMD_VENDOR_BASE;
                uint8_t control_origin_ttl = 0u;
                int reverse_route_ret;

                if (parsed.packet.msg_type == MSG_COMMAND &&
                    gateway_command_extract_id(parsed.payload,
                                               parsed.payload_len,
                                               &control_command_id) != PROTO_OK) {
                    status_debug_note("DBG_C5_CONTROL_COMMAND_ID_FAIL\n");
                    continue;
                }
                if (!app_mesh_c5_gateway_control_origin_ttl(
                        parsed.packet.msg_type,
                        (uint16_t)control_command_id,
                        &control_origin_ttl)) {
                    status_debug_printf("DBG_C5_CONTROL_ROUTE_TTL_FAIL msg=0x%02x\n",
                                        parsed.packet.msg_type);
                    continue;
                }
                reverse_route_ret = mesh_relay_note_gateway_control_reverse_route(
                    &mesh_runtime,
                    &parsed.packet,
                    parsed.previous_hop_id,
                    quality,
                    control_origin_ttl,
                    k_uptime_get_32());
                if (reverse_route_ret != PROTO_OK &&
                    reverse_route_ret != PROTO_ERR_NO_SPACE) {
                    status_debug_printf("DBG_C5_CONTROL_ROUTE_REJECT msg=0x%02x src=0x%llx prev=0x%llx ttl=%u origin_ttl=%u ret=%d\n",
                                        parsed.packet.msg_type,
                                        (unsigned long long)parsed.packet.src_id,
                                        (unsigned long long)parsed.previous_hop_id,
                                        parsed.packet.ttl,
                                        control_origin_ttl,
                                        reverse_route_ret);
                    continue;
                }
                status_debug_printf("DBG_C5_CONTROL_ROUTE_READY msg=0x%02x prev=0x%llx ttl=%u origin_ttl=%u ret=%d\n",
                                    parsed.packet.msg_type,
                                    (unsigned long long)parsed.previous_hop_id,
                                    parsed.packet.ttl,
                                    control_origin_ttl,
                                    reverse_route_ret);
            }

            if (app_mesh_c5_route_capture_requires_inline_timing_install(
                    parsed.packet.msg_type,
                    contact_purpose ==
                        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION)) {
                uint32_t received_at_ms = k_uptime_get_32();

                if (!mesh_handle_event_control(&parsed.packet,
                                               parsed.payload,
                                               parsed.payload_len,
                                               parsed.previous_hop_id,
                                               received_at_ms)) {
                    status_debug_printf("DBG_EVENT_CTRL_INLINE_FAIL msg=0x%02x\n",
                                        parsed.packet.msg_type);
                    continue;
                }
                capture_count++;
                last_ret = 0;
                status_debug_printf("DBG_EVENT_CTRL_INLINE msg=0x%02x recv=%u next=0x%llx seq=%u\n",
                                    parsed.packet.msg_type,
                                    received_at_ms,
                                    (unsigned long long)parsed.previous_hop_id,
                                    parsed.packet.seq);
                break;
            }

            if (!mesh_queue_from_frame_at_internal(frame,
                                                   frame_len,
                                                   quality,
                                                   UWB_CHANNEL_WAKE_CONTACT,
                                                   k_uptime_get_32(),
                                                   NULL,
                                                   0u,
                                                   false,
                                                   NULL,
                                                   NULL)) {
                status_debug_printf("DBG_ROUTE_REPLY_QUEUE_FAIL msg=0x%02x seq=%u\n",
                                    parsed.packet.msg_type,
                                    parsed.packet.seq);
                continue;
            }
            capture_count++;
            process_event_control_after_rx =
                app_mesh_c5_route_capture_requires_post_rx_response(
                    parsed.packet.msg_type);
            if (app_mesh_c5_route_capture_requires_ack_hold(parsed.packet.msg_type)) {
                captured_route_reply = true;
            }
            high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                 "phase=capture target=0x%016llx msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u captured=%u reason=%s",
                                 (unsigned long long)target_id,
                                 parsed.packet.msg_type,
                                 (unsigned long long)parsed.packet.src_id,
                                 (unsigned long long)parsed.packet.dst_id,
                                 (unsigned long long)parsed.previous_hop_id,
                                 parsed.packet.seq,
                                 quality,
                                 (unsigned int)capture_count,
                                 reason == NULL ? "route" : reason);
            LOG_INF("mesh route reply listen captured: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u captured=%u",
                    parsed.packet.msg_type,
                    (unsigned long long)parsed.packet.src_id,
                    (unsigned long long)parsed.packet.dst_id,
                    (unsigned long long)parsed.previous_hop_id,
                    parsed.packet.seq,
                    quality,
                    (unsigned int)capture_count);
            if (app_mesh_c5_route_capture_completes_discovery(parsed.packet.msg_type)) {
                if (route_reply_captured != NULL) {
                    *route_reply_captured = true;
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=route-captured target=0x%016llx msg=0x%02x seq=%u captured=%u reason=%s",
                                     (unsigned long long)target_id,
                                     parsed.packet.msg_type,
                                     parsed.packet.seq,
                                     (unsigned int)capture_count,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen exiting after route capture: target=0x%016llx msg=0x%02x seq=%u captured=%u",
                        (unsigned long long)target_id,
                        parsed.packet.msg_type,
                        parsed.packet.seq,
                        (unsigned int)capture_count);
                break;
            }
            if (mesh_packet_is_event_control_type(parsed.packet.msg_type)) {
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=event-control-captured target=0x%016llx msg=0x%02x seq=%u captured=%u reason=%s",
                                     (unsigned long long)target_id,
                                     parsed.packet.msg_type,
                                     parsed.packet.seq,
                                     (unsigned int)capture_count,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen exiting after event control: target=0x%016llx msg=0x%02x seq=%u captured=%u",
                        (unsigned long long)target_id,
                        parsed.packet.msg_type,
                        parsed.packet.seq,
                        (unsigned int)capture_count);
                break;
            }
            if (contact_purpose == C5_CONTACT_PURPOSE_GATEWAY_COMMAND_FLOOD &&
                parsed.packet.msg_type == MSG_COMMAND) {
                break;
            }
        }
    } else {
        last_ret = ret;
        high_debug_log_event("MESH_ROUTE_REPLY_RX",
                             "phase=config-fail target=0x%016llx ret=%d reason=%s",
                             (unsigned long long)target_id,
                             ret,
                             reason == NULL ? "route" : reason);
        LOG_WRN("mesh route reply wake-mode config failed: target=0x%016llx ret=%d reason=%s",
                (unsigned long long)target_id,
                ret,
                reason == NULL ? "route" : reason);
    }

    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    if (process_event_control_after_rx) {
        status_debug_note("DBG_EVENT_CTRL_POST_RX_HANDOFF\n");
        (void)mesh_process_queued_rx_now("route-reply-event-control");
    }
    mesh_restart_role_scan();

    if (mesh_rx_pending_count() > 0u) {
        (void)mesh_submit_work(&mesh_rx_work);
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=done target=0x%016llx captured=%u last_ret=%d reason=%s",
                         (unsigned long long)target_id,
                         (unsigned int)capture_count,
                         last_ret,
                         reason == NULL ? "route" : reason);
    status_debug_printf("DBG_ROUTE_REPLY_LISTEN_DONE captured=%u ret=%d\n",
                        (unsigned int)capture_count,
                        last_ret);
    LOG_INF("mesh route reply listen done: target=0x%016llx captured=%u last_ret=%d reason=%s",
            (unsigned long long)target_id,
            (unsigned int)capture_count,
            last_ret,
            reason == NULL ? "route" : reason);
    if (captured_route_reply) {
        status_debug_note("DBG_ROUTE_REPLY_CONTACT_KEEP_ACK\n");
    } else {
        mesh_c5_contact_clear(capture_count > 0u ? "reply-listen-done" :
                              "reply-listen-timeout");
    }
    return capture_count > 0u ? 0 : last_ret;
}

struct mesh_direct_gateway_ack_debug {
    enum dwm3000_rx_failure rx_failure;
    uint64_t previous_hop_id;
    uint16_t frame_len;
    uint8_t timeout_slices;
    bool saw_frame;
    bool matched;
    bool queued_frame;
    bool valid_frame;
};

static void mesh_debug_log_direct_gateway_ack_result(
    const char *reason,
    int ret,
    const struct mesh_direct_gateway_ack_debug *debug,
    uint8_t attempt)
{
    if (debug == NULL) {
        return;
    }

    status_debug_printf("DBG_DIRECT_GW_ACK_DONE ret=%d m=%u f=%u q=%u v=%u fail=%u sl=%u prev=0x%llx len=%u att=%u r=%s\n",
                        ret,
                        debug->matched ? 1u : 0u,
                        debug->saw_frame ? 1u : 0u,
                        debug->queued_frame ? 1u : 0u,
                        debug->valid_frame ? 1u : 0u,
                        (unsigned int)debug->rx_failure,
                        debug->timeout_slices,
                        (unsigned long long)debug->previous_hop_id,
                        debug->frame_len,
                        attempt,
                        reason == NULL ? "route" : reason);
}

static bool mesh_direct_gateway_ack_matches_packet(const struct mesh_outbound *probe,
                                                   const struct proto_packet *packet,
                                                   const uint8_t *payload,
                                                   size_t payload_len,
                                                   uint64_t previous_hop_id,
                                                   uint8_t quality)
{
    ARG_UNUSED(quality);

    if (probe == NULL || packet == NULL) {
        return false;
    }

    return app_mesh_direct_gateway_ack_matches(probe,
                                               packet,
                                               payload,
                                               payload_len,
                                               previous_hop_id,
                                               GATEWAY_ID);
}

static int mesh_wait_for_direct_gateway_ack_configured(const struct mesh_outbound *probe,
                                                       const char *reason,
                                                       bool apply_tracked_ack,
                                                       struct mesh_direct_gateway_ack_debug *debug)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    uint32_t deadline_ms;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    uint8_t timeout_slices = 0u;
    int ret = -ETIMEDOUT;

    if (probe == NULL) {
        return -EINVAL;
    }
    if (debug != NULL) {
        memset(debug, 0, sizeof(*debug));
    }

    deadline_ms = k_uptime_get_32() + MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
    ARG_UNUSED(reason);
    while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms)) {
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        bool valid_mesh_frame = false;
        uint64_t previous_hop_id = 0u;
        struct proto_packet *packet = &mesh_direct_gateway_ack_packet;
        uint8_t *payload = mesh_direct_gateway_ack_payload;
        size_t payload_len = 0u;
        uint32_t remaining_ms = uptime_ms_until_deadline(k_uptime_get_32(),
                                                         deadline_ms);
        uint32_t slice_ms;

        memset(packet, 0, sizeof(*packet));
        if (remaining_ms == 0u) {
            break;
        }
        slice_ms = MIN(remaining_ms, MESH_GATEWAY_DIRECT_PROBE_ACK_RX_SLICE_MS);
        rx_failure = DWM3000_RX_FAILURE_NONE;
        ret = dwm3000_driver_receive_frame_detailed_quiet(slice_ms,
                                                          frame,
                                                          UWB_MESH_MAX_FRAME_LEN,
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
        if (ret == -ETIMEDOUT) {
            if (timeout_slices < UINT8_MAX) {
                timeout_slices++;
            }
            if (debug != NULL) {
                debug->timeout_slices = timeout_slices;
                debug->rx_failure = rx_failure;
            }
            continue;
        }
        if (ret < 0) {
            if (debug != NULL) {
                debug->rx_failure = rx_failure;
            }
            break;
        }

        status_debug_tx_gateway_ack_rx_pulse();
        if (debug != NULL) {
            debug->saw_frame = true;
            debug->frame_len = (uint16_t)MIN(frame_len, (size_t)UINT16_MAX);
        }
        if (uwb_mesh_frame_decode(frame,
                                  frame_len,
                                  NETWORK_ID,
                                  DEVICE_ID,
                                  &previous_hop_id,
                                  packet,
                                  payload,
                                  MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP,
                                  &payload_len) == PROTO_OK &&
            mesh_direct_gateway_ack_matches_packet(probe,
                                                   packet,
                                                   payload,
                                                   payload_len,
                                                   previous_hop_id,
                                                   quality)) {
            bool local_ack = packet->dst_id == DEVICE_ID;

            if (debug != NULL) {
                debug->matched = true;
                debug->previous_hop_id = previous_hop_id;
            }
            if (apply_tracked_ack) {
                (void)mesh_ch9_tx_pending_handle_ack(packet,
                                                     payload,
                                                     payload_len,
                                                     previous_hop_id);
            }
            if (!local_ack || !apply_tracked_ack) {
                bool queued = mesh_queue_from_frame_at(frame,
                                                       frame_len,
                                                       quality,
                                                       UWB_CHANNEL_MESH_PAYLOAD,
                                                       k_uptime_get_32(),
                                                       NULL,
                                                       0u,
                                                       &valid_mesh_frame,
                                                       &previous_hop_id);

                if (debug != NULL) {
                    debug->queued_frame = true;
                    debug->valid_frame = valid_mesh_frame;
                    debug->previous_hop_id = previous_hop_id;
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_DIRECT_GW_ACK_TRANSIT queued=%u valid=%u dst=0x%llx seq=%u\n",
                                        queued ? 1u : 0u,
                                        valid_mesh_frame ? 1u : 0u,
                                        (unsigned long long)packet->dst_id,
                                        packet->seq);
                }
                if (!queued) {
                    ret = -ENOSPC;
                    break;
                }
            }
            ret = 0;
            break;
        }

        (void)mesh_queue_from_frame_at(frame,
                                       frame_len,
                                       quality,
                                       UWB_CHANNEL_MESH_PAYLOAD,
                                       k_uptime_get_32(),
                                       NULL,
                                       0u,
                                       &valid_mesh_frame,
                                       &previous_hop_id);
        if (debug != NULL) {
            debug->queued_frame = true;
            debug->valid_frame = valid_mesh_frame;
            debug->previous_hop_id = previous_hop_id;
        }
    }
    return ret == 0 ? 0 : -ETIMEDOUT;
}

static int mesh_send_direct_gateway_payload_and_wait_ack(
    const struct mesh_outbound *out,
    const char *reason,
    const struct mesh_event_plan *plan,
    uint32_t channel9_event_start_ms,
    uint32_t report_latency_reference_ms,
    bool channel9_report_latency_pending,
    bool *rf_sent)
{
    int64_t uwb_window_start_ms = -1;
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    uint32_t ack_deadline_ms;
    bool gateway_ack_required = false;
    bool ack_wait_started = false;
    bool keep_channel9_awake = false;
    int ack_ret = -ENOTCONN;
    int ret;

    if (rf_sent != NULL) {
        *rf_sent = false;
    }
    if (out == NULL ||
        out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        out->next_hop_id != GATEWAY_ID) {
        return -EINVAL;
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh direct gateway payload");
    if (ret < 0) {
        mesh_restart_role_scan();
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }

    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_mesh_payload_mode();
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_TX_CONFIG ret=%d now=%u seq=%u\n",
                            ret,
                            k_uptime_get_32(),
                            out->packet.seq);
    }
    if (ret == 0) {
        ret = mesh_send_outbound_preconfigured_ch9_locked(out,
                                                          reason == NULL ?
                                                          "direct-gateway-tx" : reason,
                                                          NULL);
    }
    if (ret == 0) {
        if (rf_sent != NULL) {
            *rf_sent = true;
        }
        mesh_relay_note_tx_sent(&mesh_runtime, out, k_uptime_get_32());
        app_mesh_paused_delivery_note_sent(&mesh_paused_delivery, out);
        (void)mesh_save_outbox_durable("single-ch9-send");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_OK\n");
        }
        if (plan != NULL) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                               out->next_hop_id,
                               plan,
                               reason);
        }
        mesh_note_channel9_local_tx(out->next_hop_id, channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms >
                                           report_latency_reference_ms ?
                                           channel9_event_start_ms -
                                           report_latency_reference_ms : 0u);
        }

        gateway_ack_required =
            (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
        if (gateway_ack_required) {
            ack_deadline_ms = k_uptime_get_32() + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
            ack_wait_started = mesh_ch9_tx_pending_track_sent(out, ack_deadline_ms);
            if (ack_wait_started) {
                mesh_ch9_event_note_persistent_ack_wait(out->next_hop_id,
                                                        mesh_ch9_tx_pending.deadline_ms,
                                                        reason);
            } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_DIRECT_GW_TX_ACK_TRANSIT_WAIT dst=0x%llx src=0x%llx seq=%u\n",
                                    (unsigned long long)out->packet.dst_id,
                                    (unsigned long long)out->packet.src_id,
                                    out->packet.seq);
            }
            ack_ret = mesh_wait_for_direct_gateway_ack_configured(out,
                                                                  reason,
                                                                  ack_wait_started,
                                                                  &ack_debug);
            status_debug_printf("DBG_DIRECT_GW_TX_ACK_WAIT ret=%d seq=%u\n",
                                ack_ret,
                                out->packet.seq);
            app_mesh_test_note_direct_gateway_ack(out->packet.seq,
                                                  ack_ret,
                                                  report_tx_queue_used());
            keep_channel9_awake =
                (ack_ret == 0 || ack_ret == -ETIMEDOUT) &&
                report_tx_queue_used() > 0u;
            if (ack_ret != 0) {
                if (ack_wait_started) {
                    mesh_ch9_tx_pending_clear();
                }
                ret = -EAGAIN;
            } else {
                route_record_success_at(&mesh_runtime.upstream, k_uptime_get_32());
                mesh_direct_gateway_bulk_probe_clear("direct-gateway-ack");
                if (mesh_report_callbacks != NULL &&
                    mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed != NULL) {
                    mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed(
                        &out->packet);
                }
            }
        }
    }

    if (keep_channel9_awake) {
        (void)dwm3000_driver_idle();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_KEEP_CH9_AWAKE q=%u seq=%u ret=%d\n",
                                report_tx_queue_used(),
                                out->packet.seq,
                                ack_ret);
        }
    } else {
        mesh_release_radio_after_mesh_turn(true, "direct-gateway-payload");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    k_mutex_unlock(&mesh_send_scratch_lock);

    if (ack_wait_started) {
        mesh_debug_log_direct_gateway_ack_result(reason, ack_ret, &ack_debug, 0u);
    }
    if (ret < 0) {
        return ret;
    }
    mesh_schedule_tx_timeout();
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_init_direct_gateway_batch_plan(struct mesh_event_plan *plan,
                                                uint32_t now_ms)
{
    if (plan == NULL) {
        return;
    }

    plan->action = MESH_EVENT_PLAN_START;
    plan->start_ms = now_ms;
    plan->window_ms = MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS;
    plan->end_ms = now_ms + MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS;
}

static int mesh_try_send_report_tx_ch9_direct_gateway_batch(
    const struct mesh_event_plan *plan,
    uint64_t next_hop_id,
    uint8_t max_sent,
    uint32_t selected_at_ms)
{
    struct mesh_event_plan tx_plan;
    struct mesh_ch9_slot_tx_context slot_tx = {
        .uwb_window_start_ms = -1,
    };
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    struct mesh_outbound *queued = &report_tx_worker_scratch;
    struct app_mesh_coordinator_decision coordinator_decision;
    uint32_t batch_id;
    uint32_t reserve_ms = MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS;
    uint32_t slot_ms;
    uint32_t now_ms;
    uint32_t deadline_ms;
    uint32_t projected_done_ms = 0u;
    uint64_t batch_src_id = 0u;
    uint8_t candidate_count = 0u;
    uint8_t sent_count = 0u;
    bool ack_wait_started = false;
    int ret;

    if (plan == NULL || next_hop_id != GATEWAY_ID || max_sent == 0u) {
        return -EINVAL;
    }

    tx_plan = *plan;
    slot_ms = uptime_ms_until_deadline(plan->start_ms, plan->end_ms);
    if (slot_ms <= reserve_ms) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_BATCH_DIRECT_NO_RESERVE\n");
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_RESERVE_FAIL now=%u start=%u end=%u slot=%u reserve=%u\n",
                                k_uptime_get_32(),
                                plan->start_ms,
                                plan->end_ms,
                                slot_ms,
                                reserve_ms);
        }
        report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return -EBUSY;
    }
    tx_plan.end_ms = plan->end_ms - reserve_ms;
    batch_id = mesh_ch9_next_batch_id();
    now_ms = k_uptime_get_32();
    deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_RESERVE batch=%u now=%u start=%u end=%u tx_end=%u reserve=%u cfg=%u guard=%u rx=%u q=%u max=%u\n",
                            batch_id,
                            now_ms,
                            plan->start_ms,
                            plan->end_ms,
                            tx_plan.end_ms,
                            reserve_ms,
                            MESH_CH9_TX_CONFIG_GUARD_MS,
                            MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                            MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS,
                            k_msgq_num_used_get(&report_tx_msgq),
                            max_sent);
    }

    while (candidate_count < max_sent) {
        uint64_t queued_next_hop_id = 0u;
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;
        bool fits;

        if (k_msgq_peek(&report_tx_msgq, queued) != 0) {
            break;
        }
        mesh_coordinator_decide_now("ch9-direct-batch-collect",
                                    &coordinator_decision);
        if (!coordinator_decision.mesh_work_allowed) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=coord n=%u state=%s q=%u\n",
                                    candidate_count,
                                    app_mesh_coordinator_state_name(
                                        coordinator_decision.state),
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }

        now_ms = k_uptime_get_32();
        if (!mesh_outbound_ready_for_tx(queued, now_ms) ||
            !mesh_packet_prefers_channel9(&queued->packet)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=blocked n=%u ready=%u ch9=%u seq=%u earliest=%u q=%u\n",
                                    candidate_count,
                                    mesh_outbound_ready_for_tx(queued, now_ms) ? 1u : 0u,
                                    mesh_packet_prefers_channel9(&queued->packet) ? 1u : 0u,
                                    queued->packet.seq,
                                    queued->earliest_tx_ms,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        ret = mesh_relay_select_next_hop(&mesh_runtime,
                                         queued->packet.dst_id,
                                         &queued_next_hop_id);
        if (ret != PROTO_OK || queued_next_hop_id != next_hop_id) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=next-hop n=%u ret=%d next=0x%llx want=0x%llx seq=%u q=%u\n",
                                    candidate_count,
                                    ret,
                                    (unsigned long long)queued_next_hop_id,
                                    (unsigned long long)next_hop_id,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        if (queued->packet.dst_id != GATEWAY_ID ||
            (queued->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
            (candidate_count > 0u && queued->packet.src_id != batch_src_id)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_STOP reason=batch-key n=%u src=0x%llx first=0x%llx dst=0x%llx flags=0x%02x seq=%u q=%u\n",
                                    candidate_count,
                                    (unsigned long long)queued->packet.src_id,
                                    (unsigned long long)batch_src_id,
                                    (unsigned long long)queued->packet.dst_id,
                                    queued->packet.flags,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        if (candidate_count == 0u) {
            batch_src_id = queued->packet.src_id;
        }

        report_tx_batch_candidates[candidate_count] = *queued;
        mesh_outbound_refresh_age(&report_tx_batch_candidates[candidate_count],
                                  now_ms);
        report_tx_batch_candidates[candidate_count].radio_channel =
            UWB_CHANNEL_MESH_PAYLOAD;
        report_tx_batch_candidates[candidate_count].next_hop_id = next_hop_id;
        ret = mesh_outbound_set_ch9_batch_metadata(
            &report_tx_batch_candidates[candidate_count],
            batch_id,
            false);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_META_FAIL ret=%d n=%u seq=%u\n",
                                    ret,
                                    candidate_count,
                                    queued->packet.seq);
            }
            break;
        }

        if (candidate_count == 0u) {
            send_start_ms = mesh_ch9_slot_send_start_ms(
                &report_tx_batch_candidates[candidate_count],
                &tx_plan,
                now_ms);
        } else {
            send_start_ms = projected_done_ms +
                MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS;
        }
        report_tx_batch_candidates[candidate_count].earliest_tx_ms =
            send_start_ms;
        fits = mesh_ch9_tx_fits_configured_slot(&report_tx_batch_candidates[candidate_count],
                                                &tx_plan,
                                                now_ms,
                                                send_start_ms,
                                                &required_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FIT batch=%u n=%u now=%u start=%u tx_end=%u txstart=%u req=%u fit=%u seq=%u len=%u\n",
                                batch_id,
                                candidate_count,
                                now_ms,
                                tx_plan.start_ms,
                                tx_plan.end_ms,
                                send_start_ms,
                                required_ms,
                                fits ? 1u : 0u,
                                report_tx_batch_candidates[candidate_count].packet.seq,
                                (unsigned int)mesh_outbound_encoded_frame_len(
                                    &report_tx_batch_candidates[candidate_count]));
        }
        if (!fits) {
            break;
        }

        ret = report_tx_queue_discard_head();
        if (ret != 0) {
            break;
        }
        projected_done_ms = send_start_ms +
            mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(
                &report_tx_batch_candidates[candidate_count]));
        candidate_count++;
    }

    if (candidate_count == 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_EMPTY batch=%u q=%u\n",
                                batch_id,
                                k_msgq_num_used_get(&report_tx_msgq));
        }
        return -EBUSY;
    }

    ret = mesh_outbound_set_ch9_batch_metadata(
        &report_tx_batch_candidates[candidate_count - 1u],
        batch_id,
        true);
    if (ret != PROTO_OK) {
        (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                0u,
                                                candidate_count,
                                                "direct-final-meta-fail");
        return mesh_errno_from_proto(ret);
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    ret = mesh_ch9_slot_tx_begin(&slot_tx);
    if (ret < 0) {
        k_mutex_unlock(&mesh_send_scratch_lock);
        (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                0u,
                                                candidate_count,
                                                "direct-config-fail");
        return ret;
    }

    projected_done_ms = 0u;
    for (uint8_t i = 0u; i < candidate_count; i++) {
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;

        now_ms = k_uptime_get_32();
        send_start_ms = (i == 0u) ?
            mesh_ch9_slot_send_start_ms(&report_tx_batch_candidates[i],
                                        &tx_plan,
                                        now_ms) :
            projected_done_ms + MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS;
        report_tx_batch_candidates[i].earliest_tx_ms = send_start_ms;
        if (!mesh_ch9_tx_fits_configured_slot(&report_tx_batch_candidates[i],
                                              &tx_plan,
                                              now_ms,
                                              send_start_ms,
                                              &required_ms)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_DIRECT_CONFIGURED_TRIM\n");
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_TRIM batch=%u keep=%u old=%u now=%u tx_end=%u req=%u seq=%u\n",
                                    batch_id,
                                    i,
                                    candidate_count,
                                    now_ms,
                                    tx_plan.end_ms,
                                    required_ms,
                                    report_tx_batch_candidates[i].packet.seq);
            }
            (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                    i,
                                                    candidate_count,
                                                    "direct-configured-trim");
            candidate_count = i;
            if (candidate_count > 0u) {
                (void)mesh_outbound_set_ch9_batch_metadata(
                    &report_tx_batch_candidates[candidate_count - 1u],
                    batch_id,
                    true);
            }
            break;
        }
        projected_done_ms = send_start_ms +
            mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(
                &report_tx_batch_candidates[i]));
    }

    if (candidate_count == 0u) {
        mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                           next_hop_id,
                           plan,
                           "queued-direct-gateway-batch-fit");
        mesh_relay_note_channel9_missed(&mesh_runtime,
                                        next_hop_id,
                                        &mesh_event_stats);
        app_mesh_test_note_ch9_missed();
        report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        mesh_ch9_slot_tx_end(&slot_tx);
        k_mutex_unlock(&mesh_send_scratch_lock);
        return -EALREADY;
    }

    mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                       next_hop_id,
                       plan,
                       "queued-direct-gateway-batch");

    uint32_t previous_frame_done_ms = 0u;
    for (uint8_t i = 0u; i < candidate_count; i++) {
        size_t sent_frame_len = 0u;
        uint32_t done_ms;

        ret = mesh_send_outbound_preconfigured_ch9_locked(
            &report_tx_batch_candidates[i],
            "queued-direct-gateway-batch",
            &sent_frame_len);
        if (ret < 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &report_tx_batch_candidates[i].packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
            uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "direct-gateway-batch-send");

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_DIRECT_SEND_FAIL\n");
                status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FAIL batch=%u ret=%d sent=%u n=%u seq=%u final=%u delay=%u\n",
                                    batch_id,
                                    ret,
                                    sent_count,
                                    candidate_count,
                                    report_tx_batch_candidates[i].packet.seq,
                                    i == candidate_count - 1u ? 1u : 0u,
                                    retry_delay_ms);
            }
            (void)mesh_requeue_ch9_batch_candidates(report_tx_batch_candidates,
                                                    i,
                                                    candidate_count,
                                                    "direct-send-fail");
            if (mesh_ch9_tx_pending.active) {
                (void)mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
                if (!mesh_ch9_tx_pending.active) {
                    mesh_ch9_tx_pending_clear();
                }
            }
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               next_hop_id,
                               plan,
                               "queued-direct-gateway-batch-send");
            mesh_ch9_slot_tx_end(&slot_tx);
            k_mutex_unlock(&mesh_send_scratch_lock);
            report_tx_schedule_backoff(retry_delay_ms,
                                       "direct-gateway-batch-send");
            return -EALREADY;
        }

        {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &report_tx_batch_candidates[i].packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                                &retry_key);
        }

        mesh_relay_note_tx_sent(&mesh_runtime,
                                &report_tx_batch_candidates[i],
                                k_uptime_get_32());
        app_mesh_paused_delivery_note_sent(&mesh_paused_delivery,
                                           &report_tx_batch_candidates[i]);
        if (mesh_ch9_tx_pending_track_sent(&report_tx_batch_candidates[i],
                                           deadline_ms)) {
            ack_wait_started = true;
        }
        sent_count++;
        done_ms = k_uptime_get_32();

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t actual_gap_ms =
                previous_frame_done_ms == 0u ? 0u :
                uptime_ms_until_deadline(previous_frame_done_ms, done_ms);

            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_FRAME batch=%u sent=%u n=%u done=%u tx_end=%u rem=%u len=%u seq=%u final=%u gap=%u\n",
                                batch_id,
                                sent_count,
                                candidate_count,
                                done_ms,
                                tx_plan.end_ms,
                                uptime_ms_until_deadline(done_ms, tx_plan.end_ms),
                                (unsigned int)sent_frame_len,
                                report_tx_batch_candidates[i].packet.seq,
                                i == candidate_count - 1u ? 1u : 0u,
                                actual_gap_ms);
        }
        previous_frame_done_ms = done_ms;
    }

    (void)mesh_save_outbox_durable("direct-ch9-batch");

    if (ack_wait_started) {
        int ack_ret;

        mesh_ch9_event_note_persistent_ack_wait(next_hop_id,
                                                deadline_ms,
                                                "queued-direct-gateway-batch");
        ack_ret = mesh_wait_for_direct_gateway_ack_configured(
            &report_tx_batch_candidates[candidate_count - 1u],
            "queued-direct-gateway-batch",
            true,
            &ack_debug);
        mesh_debug_log_direct_gateway_ack_result("queued-direct-gateway-batch",
                                                 ack_ret,
                                                 &ack_debug,
                                                 0u);
        app_mesh_test_note_direct_gateway_ack(
            report_tx_batch_candidates[candidate_count - 1u].packet.seq,
            ack_ret,
            report_tx_queue_used());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_ACK batch=%u ret=%d sent=%u deadline=%u active=%u\n",
                                batch_id,
                                ack_ret,
                                sent_count,
                                deadline_ms,
                                mesh_ch9_tx_pending.active ? 1u : 0u);
        }
        if (ack_ret == 0) {
            route_record_success_at(&mesh_runtime.upstream, k_uptime_get_32());
            mesh_direct_gateway_bulk_probe_clear("direct-gateway-batch-ack");
        } else {
            mesh_schedule_tx_timeout();
        }
    } else {
        report_tx_schedule(0u);
    }

    mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);

    mesh_note_channel9_local_tx(next_hop_id, plan->start_ms);
    mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                       next_hop_id,
                       plan,
                       "queued-direct-gateway-batch");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_SENT batch=%u selected_at=%u sent=%u ack=%u deadline=%u q=%u\n",
                            batch_id,
                            selected_at_ms,
                            sent_count,
                            ack_wait_started ? 1u : 0u,
                            deadline_ms,
                            report_tx_queue_used());
    }
    if (ack_wait_started) {
        mesh_schedule_tx_timeout();
    }
    return 0;
}
#endif

static int mesh_send_direct_gateway_probe_and_wait(const struct mesh_outbound *probe,
                                                   const char *reason,
                                                   bool apply_tracked_ack,
                                                   uint8_t attempt,
                                                   bool bounded_scratch_acquire)
{
    int64_t uwb_window_start_ms = -1;
    struct mesh_direct_gateway_ack_debug ack_debug = {0};
    bool ack_wait_ran = false;
    int ret;

    if (probe == NULL ||
        probe->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        probe->next_hop_id != GATEWAY_ID) {
        return -EINVAL;
    }

    app_mesh_direct_probe_breadcrumb_note(APP_MESH_DIRECT_PROBE_PHASE_SCRATCH_ACQUIRE,
                                          attempt,
                                          probe->packet.seq);
    ret = k_mutex_lock(&mesh_send_scratch_lock,
                       bounded_scratch_acquire ?
                           K_MSEC(APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS) :
                           K_FOREVER);
    if (ret < 0) {
        return -EBUSY;
    }
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_SCAN_GUARD_TRANSITION,
        attempt,
        probe->packet.seq);
    mesh_stop_role_scan();
    ret = mesh_transport_radio_start("mesh direct gateway route probe");
    if (ret < 0) {
        mesh_restart_role_scan();
        k_mutex_unlock(&mesh_send_scratch_lock);
        return ret;
    }

    uwb_window_start_ms = k_uptime_get();
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_CH9_CONFIGURE_RECOVERY,
        attempt,
        probe->packet.seq);
    ret = dwm3000_driver_configure_mesh_payload_mode();
    app_mesh_direct_probe_breadcrumb_note(
        APP_MESH_DIRECT_PROBE_PHASE_PAYLOAD_MODE_COMPLETE,
        attempt,
        probe->packet.seq);
    mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_PROBE_CONFIG ret=%d now=%u attempt=%u\n",
                            ret,
                            k_uptime_get_32(),
                            attempt);
    }
    if (ret == 0) {
        ret = mesh_send_outbound_preconfigured_ch9_locked(
            probe,
            "gateway-direct-route-probe",
            NULL);
    }
    if (ret == 0) {
        ack_wait_ran = true;
        ret = mesh_wait_for_direct_gateway_ack_configured(probe,
                                                          reason,
                                                          apply_tracked_ack,
                                                          &ack_debug);
    } else {
        status_debug_printf("DBG_DIRECT_GW_PROBE_TX_FAIL ret=%d attempt=%u\n",
                            ret,
                            attempt);
    }

    if (ret == 0) {
        (void)dwm3000_driver_idle();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_PROBE_KEEP_CH9_AWAKE seq=%u attempt=%u\n",
                                probe->packet.seq,
                                attempt);
        }
    } else {
        mesh_release_radio_after_mesh_turn(true, "direct-gateway-probe");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    k_mutex_unlock(&mesh_send_scratch_lock);
    if (ack_wait_ran) {
        mesh_debug_log_direct_gateway_ack_result(reason, ret, &ack_debug, attempt);
    }
    return ret;
}

static int mesh_try_direct_gateway_route_probe(uint64_t target_id,
                                               const char *reason,
                                               bool install_direct_route,
                                               enum app_mesh_direct_gateway_retry_mode retry_mode,
                                               uint32_t survey_id)
{
    struct mesh_outbound *probe = &mesh_direct_gateway_probe_scratch;
    struct app_mesh_direct_gateway_retry_state retry_state;
    struct app_mesh_direct_gateway_retry_decision retry_decision;
    uint32_t policy_deadline_ms = 0u;
    int last_ret = -ETIMEDOUT;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY ||
        target_id != GATEWAY_ID) {
        return -ENOTSUP;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ)) {
        install_direct_route = false;
    }
    if (app_mesh_direct_gateway_retry_init(&retry_state, retry_mode,
                                           DEVICE_ID, survey_id) != 0) {
        return -EINVAL;
    }
    if (retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
        policy_deadline_ms = k_uptime_get_32() +
            app_mesh_direct_gateway_retry_policy_horizon_ms(
                retry_mode, SURVEY_DIRECT_GATEWAY_PROBE_ATTEMPT_MS,
                APP_MESH_DIRECT_GATEWAY_SURVEY_SCRATCH_ACQUIRE_MS);
    }

    while ((ret = k_mutex_lock(&mesh_direct_gateway_probe_scratch_lock,
                               K_NO_WAIT)) < 0) {
        if (retry_mode != APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY) {
            return -EBUSY;
        }
        (void)app_mesh_direct_gateway_retry_note(
            &retry_state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY, 0u,
            &retry_decision);
        if (!retry_decision.retry) {
            status_debug_printf("DBG_DIRECT_GW_PROBE_BUSY_TERMINAL attempts=%u deferrals=%u phase=outer-scratch\n",
                                retry_state.attempts,
                                retry_state.busy_deferrals);
            return -EBUSY;
        }
        if (retry_decision.delay_ms >
            app_mesh_direct_gateway_retry_deadline_remaining_ms(
                k_uptime_get_32(), policy_deadline_ms)) {
            return -ETIMEDOUT;
        }
        k_msleep(retry_decision.delay_ms);
    }

    while (true) {
        uint32_t now_ms = k_uptime_get_32();
        uint8_t attempt = retry_state.attempts + 1u;

        memset(probe, 0, sizeof(*probe));
        probe->packet.msg_type = MSG_GATEWAY_ROUTE_REQ;
        probe->packet.flags = FLAG_GATEWAY_ACK_REQUIRED;
        probe->packet.src_id = DEVICE_ID;
        probe->packet.dst_id = GATEWAY_ID;
        probe->packet.session_id = nonzero_uptime_session_id();
        probe->packet.seq = mesh_next_event_control_seq();
        probe->packet.ttl = MESH_DEFAULT_TTL;
        probe->packet.payload_len = 0u;
        probe->payload_len = 0u;
        probe->next_hop_id = GATEWAY_ID;
        probe->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        probe->queued_at_ms = now_ms;
        probe->earliest_tx_ms = now_ms;

        status_debug_printf("DBG_DIRECT_GW_PROBE_TX seq=%u now=%u attempt=%u/%u reason=%s\n",
                            probe->packet.seq,
                            now_ms,
                            (unsigned int)attempt,
                            (unsigned int)(retry_mode ==
                                APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY ?
                                APP_MESH_DIRECT_GATEWAY_SURVEY_ATTEMPTS :
                                APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS),
                            reason == NULL ? "route" : reason);
        ret = mesh_send_direct_gateway_probe_and_wait(probe,
                                                      reason,
                                                      false,
                                                      attempt,
                                                      retry_mode ==
                                                          APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY);
        if (ret == 0) {
            (void)app_mesh_direct_gateway_retry_note(
                &retry_state, APP_MESH_DIRECT_GATEWAY_ATTEMPT_SUCCESS, 0u,
                &retry_decision);
            if (!install_direct_route) {
                status_debug_note("DBG_DIRECT_GW_PROBE_CONTACT_ONLY\n");
                high_debug_log_event("MESH_DIRECT_GATEWAY_PROBE",
                                     "target=0x%016llx attempt=%u mode=contact-only reason=%s",
                                     (unsigned long long)target_id,
                                     (unsigned int)attempt,
                                     reason == NULL ? "route" : reason);
                last_ret = 0;
                goto out_unlock;
            }

            ret = mesh_relay_note_direct_gateway_route(&mesh_runtime,
                                                       k_uptime_get_32());
            if (ret != PROTO_OK) {
                status_debug_printf("DBG_DIRECT_GW_ROUTE_INSTALL_FAIL ret=%d\n", ret);
                last_ret = mesh_errno_from_proto(ret);
                goto out_unlock;
            }

            status_debug_note("DBG_DIRECT_GW_ROUTE_READY\n");
            high_debug_log_event("MESH_DIRECT_GATEWAY_ROUTE",
                                 "target=0x%016llx attempt=%u reason=%s",
                                 (unsigned long long)target_id,
                                 (unsigned int)attempt,
                                 reason == NULL ? "route" : reason);
            last_ret = 0;
            goto out_unlock;
        }
        last_ret = ret;
        status_debug_printf("DBG_DIRECT_GW_PROBE_MISS ret=%d attempt=%u\n",
                            ret,
                            (unsigned int)attempt);
        (void)app_mesh_direct_gateway_retry_note(
            &retry_state,
            ret == -EBUSY ? APP_MESH_DIRECT_GATEWAY_ATTEMPT_RF_BUSY :
                            APP_MESH_DIRECT_GATEWAY_ATTEMPT_FAILED,
            retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE &&
                retry_state.attempts + 1u <
                    APP_MESH_DIRECT_GATEWAY_ROUTE_ATTEMPTS ?
                sys_rand32_get() : 0u,
            &retry_decision);
        if (!retry_decision.retry) {
            if (retry_decision.busy_exhausted) {
                status_debug_printf("DBG_DIRECT_GW_PROBE_BUSY_TERMINAL attempts=%u deferrals=%u phase=radio\n",
                                    retry_state.attempts,
                                    retry_state.busy_deferrals);
            }
            break;
        }
        status_debug_printf("DBG_DIRECT_GW_PROBE_BACKOFF ms=%u next_attempt=%u busy_defer=%u\n",
                            retry_decision.delay_ms,
                            (unsigned int)(retry_state.attempts + 1u),
                            retry_decision.attempt_consumed ? 0u : 1u);
        if (retry_mode == APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY &&
            retry_decision.delay_ms >
                app_mesh_direct_gateway_retry_deadline_remaining_ms(
                    k_uptime_get_32(), policy_deadline_ms)) {
            last_ret = -ETIMEDOUT;
            break;
        }
        k_msleep(retry_decision.delay_ms);
    }

out_unlock:
    k_mutex_unlock(&mesh_direct_gateway_probe_scratch_lock);
    return last_ret;
}

static bool mesh_route_request_defer_rf_busy(
    enum app_mesh_route_request_rf_phase phase,
    const char *phase_name,
    int ret,
    bool embedded_route_sent,
    const char *reason,
    const struct mesh_outbound *route_request,
    uint64_t target_id,
    const struct mesh_route_discovery_state *route_discovery_before_prepare)
{
    struct app_mesh_route_request_rf_failure_decision decision;
    struct app_mesh_rf_retry_key retry_key;
    uint32_t retry_delay_ms;

    app_mesh_route_request_rf_failure_decide(phase,
                                             ret,
                                             embedded_route_sent,
                                             -EBUSY,
                                             &decision);
    if (!decision.defer_retry) {
        return false;
    }

    if (decision.restore_prepared_attempt &&
        route_discovery_before_prepare != NULL) {
        mesh_runtime.route_discovery = *route_discovery_before_prepare;
    }
    retry_key = mesh_route_request_rf_retry_key(route_request, target_id,
                                                phase);
    retry_delay_ms = mesh_rf_retry_next_delay_ms(
        mesh_route_request_rf_retry_state(phase),
        &retry_key,
        APP_MESH_RF_RETRY_POLICY_WAKE_TRAIN,
        phase_name);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RF_BUSY_DEFER phase=%s ret=%d restored=%u delay=%u active=%u attempts=%u next=%u\n",
                            phase_name == NULL ? "route" : phase_name,
                            ret,
                            decision.restore_prepared_attempt ? 1u : 0u,
                            retry_delay_ms,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }
    mesh_schedule_route_waiting_retry_after(reason == NULL ? "route-rf-busy" :
                                           reason,
                                           retry_delay_ms);
    return true;
}

static int mesh_request_route_owned(
    uint64_t target_id,
    const char *reason,
    enum app_mesh_route_wait_tx_owner owner,
    const struct proto_packet *owner_packet,
    uint32_t generation)
{
    struct mesh_outbound route_req;
    struct app_mesh_route_request_policy_state route_policy_state = {0};
    struct app_mesh_route_request_policy_decision route_policy;
    struct mesh_route_capture_identity route_identity = {0};
    struct app_mesh_route_retry_identity retry_identity;
    struct mesh_route_discovery_state route_discovery_before_prepare;
    struct mesh_event_timing proposed_timing = {0};
    const struct mesh_event_timing *proposed_timing_ptr = NULL;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = now_ms;
    uint32_t route_reply_window_ms = MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    uint32_t route_random_value;
    bool embedded_route_sent = false;
    bool relay_required_route_req =
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_RELAY_REQUIRED_ROUTE_REQ) &&
        target_id == GATEWAY_ID;
    uint8_t route_request_flags;
    uint16_t route_reply_rx_delay_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
                                       MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS : 0u;
    int ret;

    if (owner == APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC) {
        owner_packet = mesh_route_waiting_tx_valid ?
                       &mesh_route_waiting_tx.packet : NULL;
        generation = owner_packet == NULL ? 0u : owner_packet->session_id;
    }
    app_mesh_route_retry_identity_select(owner, owner_packet, generation,
                                         &retry_identity);

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_CALL target=0x%llx reason=%s active=%u attempts=%u next=%u\n",
                            (unsigned long long)target_id,
                            reason == NULL ? "route" : reason,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }

    if (!mesh_coordinator_mesh_work_allowed(reason == NULL ? "route-request" :
                                            reason)) {
        mesh_schedule_route_waiting_retry(reason);
        return -EBUSY;
    }

    route_policy_state.relay_required = relay_required_route_req;
    route_policy_state.direct_bulk_suppressed =
        mesh_direct_gateway_bulk_probe_install_suppressed(target_id, now_ms);
    route_policy_state.direct_probe_ret = -ENOTCONN;
    app_mesh_route_request_policy_decide(&route_policy_state, &route_policy);
    route_request_flags = route_policy.route_request_flags;
    if (route_policy_state.direct_bulk_suppressed &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DIRECT_BULK_SUPPRESSED target=0x%llx until=%u reason=%s\n",
                            (unsigned long long)target_id,
                            mesh_direct_gateway_bulk_suppressed_until_ms,
                            reason == NULL ? "route" : reason);
    }

    ret = mesh_try_direct_gateway_route_probe(
        target_id, reason, route_policy.install_direct_route_from_probe,
        retry_identity.mode,
        retry_identity.survey_id);
    route_policy_state.direct_probe_ret = ret;
    app_mesh_route_request_policy_decide(&route_policy_state, &route_policy);
    app_mesh_test_note_direct_gateway_route_probe(target_id, ret);
    if (route_policy.direct_probe_satisfies_request) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REQ_DIRECT_READY\n");
        }
        app_mesh_test_note_route_ready(target_id, GATEWAY_ID, 0);
        mesh_schedule_route_waiting_retry_after("gateway-direct-route", 0u);
        return 0;
    }
    if (ret == 0 && !route_policy.install_direct_route_from_probe &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note(relay_required_route_req ?
                          "DBG_ROUTE_REQ_DIRECT_CONTACT_RELAY_REQUIRED\n" :
                          "DBG_ROUTE_REQ_DIRECT_CONTACT_BULK_SUPPRESSED\n");
    }
    if (relay_required_route_req && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RELAY_REQUIRED target=0x%llx reason=%s\n",
                            (unsigned long long)target_id,
                            reason == NULL ? "route" : reason);
    }
    now_ms = k_uptime_get_32();
    timing_reference_ms = now_ms;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE != ROLE_GATEWAY) {
        ret = mesh_prepare_event_timing(&proposed_timing, timing_reference_ms);
        if (ret == PROTO_OK) {
            mesh_event_timing_set_local_first_slot_tx(&proposed_timing, true);
            proposed_timing_ptr = &proposed_timing;
            status_debug_printf("DBG_ROUTE_REQ_CH9_TIMING ref=%u next=%u guard=%u win=%u\n",
                                timing_reference_ms,
                                proposed_timing.next_event_time_ms,
                                proposed_timing.guard_ms,
                                proposed_timing.event_window_ms);
        } else {
            status_debug_printf("DBG_ROUTE_REQ_CH9_TIMING_FAIL ret=%d\n", ret);
        }
    }

    route_random_value = sys_rand32_get();
    route_discovery_before_prepare = mesh_runtime.route_discovery;
    ret = mesh_relay_prepare_route_request_with_timing_flags(&mesh_runtime,
                                                             target_id,
                                                             proposed_timing_ptr,
                                                             timing_reference_ms,
                                                             route_request_flags,
                                                             route_reply_rx_delay_ms,
                                                             now_ms,
                                                             route_random_value,
                                                             &route_req);
    if (ret != PROTO_OK) {
        app_mesh_test_note_route_request_prepare_result(target_id, ret);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_PREP_FAIL ret=%d target=0x%llx active=%u attempts=%u next=%u\n",
                                ret,
                                (unsigned long long)target_id,
                                mesh_runtime.route_discovery.active ? 1u : 0u,
                                mesh_runtime.route_discovery.attempts,
                                mesh_runtime.route_discovery.next_request_ms);
        }
        if (ret == PROTO_ERR_BUSY) {
            LOG_INF("mesh route discovery backoff active: target=0x%016llx attempts=%u next_ms=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    mesh_runtime.route_discovery.next_request_ms,
                    reason);
            mesh_schedule_route_waiting_retry(reason);
            return -EAGAIN;
        }
        if (ret == PROTO_ERR_STALE) {
            LOG_WRN("mesh route discovery exhausted: target=0x%016llx attempts=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    reason);
            mesh_relay_reset_route_discovery(&mesh_runtime);
            return -ETIMEDOUT;
        }
        return mesh_errno_from_proto(ret);
    }
    if (!mesh_route_capture_identity_from_request(&route_req,
                                                  target_id,
                                                  &route_identity)) {
        LOG_ERR("mesh route request identity missing: target=0x%016llx session=%u",
                (unsigned long long)target_id,
                route_req.packet.session_id);
        return -EINVAL;
    }
    app_mesh_test_note_route_request_attempt(target_id,
                                             mesh_runtime.route_discovery.attempts,
                                             route_req.packet.ttl);
    route_reply_window_ms = mesh_route_reply_listen_window_ms(route_req.packet.ttl);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t route_backoff_ms =
            uptime_ms_until_deadline(now_ms,
                                     mesh_runtime.route_discovery.next_request_ms);

        status_debug_printf("DBG_ROUTE_REQ_BACKOFF attempt=%u ttl=%u rand=%u delay=%u next=%u\n",
                            mesh_runtime.route_discovery.attempts,
                            route_req.packet.ttl,
                            route_random_value,
                            route_backoff_ms,
                            mesh_runtime.route_discovery.next_request_ms);
        status_debug_printf("DBG_ROUTE_REPLY_WINDOW ttl=%u win=%u base=%u attempt=%u\n",
                            route_req.packet.ttl,
                            route_reply_window_ms,
                            MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
                            mesh_runtime.route_discovery.attempts);
    }

    LOG_INF("mesh route discovery request: target=0x%016llx attempt=%u next_ms=%u reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            mesh_runtime.route_discovery.next_request_ms,
            reason);
    ret = mesh_send_route_wake_train(target_id,
                                     &route_req,
                                     &embedded_route_sent,
                                     C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                     reason);
    if (ret < 0) {
        LOG_WRN("mesh route discovery wake train failed: target=0x%016llx attempt=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                ret,
                reason);
        if (mesh_route_request_defer_rf_busy(
                APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN,
                "wake",
                ret,
                embedded_route_sent,
                reason,
                &route_req,
                target_id,
                &route_discovery_before_prepare)) {
            return ret;
        }
        app_mesh_rf_retry_reset(&mesh_route_request_wake_rf_retry);
        mesh_restart_role_scan();
        mesh_schedule_route_waiting_retry(reason);
        return ret;
    }
    {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_route_request_rf_retry_key(
                &route_req,
                target_id,
                APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN);

        app_mesh_rf_retry_note_success(&mesh_route_request_wake_rf_retry,
                                       &retry_key);
    }
    if (embedded_route_sent) {
        bool route_reply_captured = false;
        int listen_ret;

        high_debug_log_event("MESH_ROUTE_REQ_TX",
                             "phase=embedded-listen target=0x%016llx attempt=%u reason=%s",
                             (unsigned long long)target_id,
                             mesh_runtime.route_discovery.attempts,
                             reason == NULL ? "route" : reason);
        listen_ret = mesh_listen_for_route_reply(target_id,
                                                 "embedded-route-request",
                                                 route_reply_window_ms,
                                                 &route_identity,
                                                 &route_reply_captured);
        if (route_reply_captured) {
            status_debug_note("DBG_EMBEDDED_ROUTE_REQ_SKIP_FALLBACK\n");
            high_debug_log_event("MESH_ROUTE_REQ_TX",
                                 "phase=embedded-reply target=0x%016llx attempt=%u reason=%s",
                                 (unsigned long long)target_id,
                                 mesh_runtime.route_discovery.attempts,
                                 reason == NULL ? "route" : reason);
            mesh_route_reply_handoff_after_capture(target_id, reason);
            return 0;
        }
        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh embedded route reply listen failed: target=0x%016llx attempt=%u ret=%d reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    listen_ret,
                    reason == NULL ? "route" : reason);
        }
        status_debug_note("DBG_EMBEDDED_ROUTE_REQ_FALLBACK\n");
        high_debug_log_event("MESH_ROUTE_REQ_TX",
                             "phase=embedded-fallback target=0x%016llx attempt=%u listen_ret=%d reason=%s",
                             (unsigned long long)target_id,
                             mesh_runtime.route_discovery.attempts,
                             listen_ret,
                             reason == NULL ? "route" : reason);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        high_debug_log_event("MESH_ROUTE_REQ_TX",
                             "phase=turnaround target=0x%016llx delay_ms=%u reason=%s",
                             (unsigned long long)target_id,
                             MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
                             reason == NULL ? "route" : reason);
        LOG_INF("mesh route channel-5 turnaround delay: target=0x%016llx delay_ms=%u reason=%s",
                (unsigned long long)target_id,
                MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
                reason == NULL ? "route" : reason);
        k_msleep(MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
    }
    high_debug_log_event("MESH_ROUTE_REQ_TX",
                         "phase=start target=0x%016llx attempt=%u reason=%s",
                         (unsigned long long)target_id,
                         mesh_runtime.route_discovery.attempts,
                         reason == NULL ? "route" : reason);
    mesh_c5_contact_exchange(target_id,
                             C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                             k_uptime_get_32() + route_reply_window_ms,
                             reason);
    ret = mesh_send_outbound(&route_req, "route-request-control");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_CONTROL_TX ret=%d seq=%u ttl=%u\n",
                            ret,
                            route_req.packet.seq,
                            route_req.packet.ttl);
    }
    high_debug_log_event("MESH_ROUTE_REQ_TX",
                         "phase=done target=0x%016llx attempt=%u ret=%d reason=%s",
                         (unsigned long long)target_id,
                         mesh_runtime.route_discovery.attempts,
                         ret,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route discovery request TX done: target=0x%016llx attempt=%u ret=%d reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            ret,
            reason == NULL ? "route" : reason);
    if (ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_route_request_rf_retry_key(
                &route_req,
                target_id,
                APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX);
        bool route_reply_captured = false;
        int listen_ret = mesh_listen_for_route_reply(target_id,
                                                     reason,
                                                     route_reply_window_ms,
                                                     &route_identity,
                                                     &route_reply_captured);

        app_mesh_rf_retry_note_success(&mesh_route_request_control_rf_retry,
                                       &retry_key);

        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh route reply listen failed after request TX: target=0x%016llx attempt=%u ret=%d reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    listen_ret,
                    reason == NULL ? "route" : reason);
        }
        if (route_reply_captured) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_REQ_REPLY_CAPTURED\n");
            }
            mesh_route_reply_handoff_after_capture(target_id, reason);
        } else {
            app_mesh_test_note_route_reply_miss(target_id, listen_ret);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REQ_REPLY_MISS listen_ret=%d win=%u\n",
                                    listen_ret,
                                    route_reply_window_ms);
            }
            mesh_schedule_route_waiting_retry(reason);
        }
    } else {
        LOG_WRN("mesh route discovery request TX failed: target=0x%016llx attempt=%u next_ms=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                mesh_runtime.route_discovery.next_request_ms,
                ret,
                reason);
        if (!mesh_route_request_defer_rf_busy(
                APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX,
                "control",
                ret,
                embedded_route_sent,
                reason,
                &route_req,
                target_id,
                &route_discovery_before_prepare)) {
            app_mesh_rf_retry_reset(&mesh_route_request_control_rf_retry);
            mesh_schedule_route_waiting_retry(reason);
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_RET ret=%d active=%u attempts=%u next=%u\n",
                            ret,
                            mesh_runtime.route_discovery.active ? 1u : 0u,
                            mesh_runtime.route_discovery.attempts,
                            mesh_runtime.route_discovery.next_request_ms);
    }
    return ret;
}

int mesh_request_route(uint64_t target_id, const char *reason)
{
    return mesh_request_route_owned(
        target_id, reason,
        APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC,
        NULL, 0u);
}

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet)
{
    return mesh_relay_packet_requires_channel9_payload_event(packet);
}

static bool mesh_outbound_needs_result_offer(const struct mesh_outbound *out)
{
    struct command_result_id result_id;

    return out != NULL &&
           out->packet.msg_type == MSG_COMMAND_RESULT &&
           out->payload_len > COLLECTION_RESULT_INLINE_C5_MAX_BYTES &&
           command_result_id_from_tlvs(out->payload, out->payload_len, &result_id) == PROTO_OK;
}

void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements)
{
    uint32_t now_ms;

    if (requirements == NULL) {
        return;
    }

    memset(requirements, 0, sizeof(*requirements));
    requirements->retune_guard_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ?
                                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS :
                                    MESH_EVENT_DEFAULT_GUARD_MS;
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        now_ms = k_uptime_get_32();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            mesh_channel9_connection_count() > 0u) {
            requirements->next_required_scan_start_ms = 0u;
        } else {
            requirements->next_required_scan_start_ms = now_ms + anchor_uwb_scan_interval_ms;
        }
        requirements->click_epoch_active = anchor_uwb_window_active();
        requirements->discovery_active = requirements->click_epoch_active;
        requirements->ranging_active = requirements->click_epoch_active;
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        now_ms = k_uptime_get_32();
        if (mesh_gateway_route_test_preempt_active(now_ms)) {
            requirements->active_until_ms = mesh_gateway_route_preempt_deadline_ms;
            requirements->discovery_active = true;
        }
    }
}

static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms)
{
    const struct mesh_event_params params = {
        .event_interval_ms = MESH_EVENT_DEFAULT_INTERVAL_MS,
        .event_window_ms = MESH_EVENT_DEFAULT_WINDOW_MS,
        .first_event_time_ms = mesh_route_test_first_event_time_ms(now_ms),
        .guard_ms = MESH_EVENT_DEFAULT_GUARD_MS,
        .peer_clock_skew_estimate_ppm = 0,
        .max_missed_events = MESH_EVENT_DEFAULT_MAX_MISSED,
        .supervision_timeout_ms = MESH_EVENT_DEFAULT_SUPERVISION_MS,
    };

    return mesh_event_timing_negotiate(timing, &params, true);
}

static uint32_t mesh_event_control_tx_reference_ms(uint8_t msg_type, uint32_t now_ms)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        msg_type == MSG_MESH_EVENT_PROPOSE) {
        return now_ms + MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
    }
    return now_ms;
}

static uint32_t mesh_event_control_rx_reference_ms(uint32_t received_at_ms)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        MESH_EVENT_CONTROL_CH5_AIRTIME_MS == 0u) {
        return received_at_ms;
    }
    if (received_at_ms <= MESH_EVENT_CONTROL_CH5_AIRTIME_MS) {
        return 1u;
    }
    return received_at_ms - MESH_EVENT_CONTROL_CH5_AIRTIME_MS;
}

static uint32_t mesh_route_test_first_event_time_ms(uint32_t now_ms)
{
    uint32_t default_first_ms = now_ms + MESH_EVENT_DEFAULT_FIRST_DELAY_MS;
    uint32_t selected_ms = default_first_ms;
    bool selected = false;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS == 0u) {
        return default_first_ms;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        uint32_t candidate_ms;

        if (!entry->valid ||
            entry->timing.event_interval_ms != MESH_EVENT_DEFAULT_INTERVAL_MS) {
            continue;
        }

        candidate_ms = entry->timing.next_event_time_ms +
                       MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS;
        while (!uptime_deadline_reached(candidate_ms, default_first_ms)) {
            candidate_ms += entry->timing.event_interval_ms;
        }
        if (!selected || uptime_deadline_reached(selected_ms, candidate_ms)) {
            selected_ms = candidate_ms;
            selected = true;
        }
    }

    if (selected) {
        high_debug_log_event("MESH_EVENT_TIMING",
                             "phase=second-slot first_ms=%u default_ms=%u offset_ms=%u",
                             selected_ms,
                             default_first_ms,
                             MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS);
    }
    return selected_ms;
}

static struct app_mesh_event_request_identity mesh_event_request_identity(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    return (struct app_mesh_event_request_identity) {
        .source_id = packet == NULL ? 0u : packet->src_id,
        .session_id = packet == NULL ? 0u : packet->session_id,
        .sequence = packet == NULL ? 0u : packet->seq,
        .payload_fingerprint = app_mesh_event_payload_fingerprint(payload,
                                                                  payload_len),
        .payload_len = payload_len > UINT16_MAX ? UINT16_MAX :
                       (uint16_t)payload_len,
        .message_type = packet == NULL ? 0u : packet->msg_type,
    };
}

static bool mesh_event_accept_timing_compatible(
    const struct mesh_event_timing *accepted,
    const struct mesh_event_control_record *proposal)
{
    const struct mesh_event_timing *proposed;

    if (accepted == NULL || proposal == NULL || !proposal->valid) {
        return false;
    }
    proposed = &proposal->timing;
    return accepted->mesh_channel == proposed->mesh_channel &&
           accepted->event_interval_ms == proposed->event_interval_ms &&
           accepted->event_window_ms == proposed->event_window_ms &&
           accepted->event_counter == proposed->event_counter &&
           accepted->guard_ms == proposed->guard_ms &&
           accepted->peer_clock_skew_estimate_ppm ==
               proposed->peer_clock_skew_estimate_ppm &&
           accepted->max_missed_events == proposed->max_missed_events &&
           accepted->supervision_timeout_ms ==
               proposed->supervision_timeout_ms;
}

static int mesh_prepare_event_control_record(
    struct mesh_event_control_record *record,
    uint64_t peer_id,
    uint8_t msg_type,
    const struct mesh_event_timing *accepted_timing,
    uint32_t session_id,
    uint16_t sequence)
{
    struct mesh_event_timing timing = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = mesh_event_control_tx_reference_ms(msg_type,
                                                                      now_ms);
    int ret;

    if (record == NULL || !mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID ||
        (msg_type != MSG_MESH_EVENT_PROPOSE &&
         msg_type != MSG_MESH_EVENT_ACCEPT)) {
        return -EINVAL;
    }

    memset(record, 0, sizeof(*record));
    if (accepted_timing != NULL) {
        timing = *accepted_timing;
    } else {
        ret = mesh_prepare_event_timing(&timing, timing_reference_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
    }
    if (msg_type == MSG_MESH_EVENT_PROPOSE) {
        mesh_event_timing_set_local_first_slot_tx(&timing, true);
    }
    ret = mesh_append_event_timing_tlvs_at(
        record->payload,
        sizeof(record->payload),
        &payload_len,
        &timing,
        now_ms);
    if (ret != PROTO_OK || payload_len > UINT8_MAX) {
        return -EMSGSIZE;
    }
    ret = mesh_init_event_control(&record->packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  session_id == 0u ?
                                      nonzero_uptime_session_id() : session_id,
                                  sequence == 0u ?
                                      mesh_next_event_control_seq() : sequence,
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    record->timing = timing;
    record->peer_id = peer_id;
    record->encoded_delay_ms = uptime_ms_until_deadline(
        now_ms, timing.next_event_time_ms);
    record->payload_len = (uint8_t)payload_len;
    record->valid = true;
    return 0;
}

static int mesh_send_event_control_record(
    const struct mesh_event_control_record *record,
    enum mesh_c5_control_send_mode mode,
    const char *reason,
    bool *rf_started,
    struct mesh_event_timing *transmitted_timing)
{
    struct mesh_outbound *outbound = &mesh_event_control_retry_scratch;
    int ret;

    if (rf_started != NULL) {
        *rf_started = false;
    }
    if (record == NULL || !record->valid) {
        return -EINVAL;
    }
    ret = k_mutex_lock(&mesh_event_control_retry_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        return -EBUSY;
    }

    memset(outbound, 0, sizeof(*outbound));
    outbound->packet = record->packet;
    memcpy(outbound->payload, record->payload, record->payload_len);
    outbound->payload_len = record->payload_len;
    outbound->next_hop_id = record->peer_id;
    outbound->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
    ret = mesh_send_c5_control_attempt(
        outbound,
        C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
        mode,
        reason == NULL ? "mesh-event-control" : reason,
        rf_started);
    if (ret == 0 && transmitted_timing != NULL) {
        uint32_t tx_done_ms = k_uptime_get_32();

        *transmitted_timing = record->timing;
        mesh_event_timing_reanchor_after_control_tx(
            transmitted_timing,
            tx_done_ms,
            record->encoded_delay_ms,
            MESH_EVENT_CONTROL_CH5_AIRTIME_MS);
    }
    k_mutex_unlock(&mesh_event_control_retry_scratch_lock);
    return ret;
}

static int mesh_send_event_control(uint64_t peer_id,
                                   uint8_t msg_type,
                                   const struct mesh_event_timing *accepted_timing,
                                   bool install_local,
                                   const char *reason,
                                   struct mesh_event_timing *transmitted_timing)
{
    struct mesh_event_timing timing = {0};
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = mesh_event_control_tx_reference_ms(msg_type, now_ms);
    uint32_t encoded_delay_ms = 0u;
    bool close_event = msg_type == MSG_MESH_EVENT_END;
    int ret;

    if (!mesh_id_is_unicast(peer_id) || peer_id == DEVICE_ID) {
        return -EINVAL;
    }

    if (!close_event) {
        if (accepted_timing != NULL) {
            timing = *accepted_timing;
        } else {
            ret = mesh_prepare_event_timing(&timing, timing_reference_ms);
            if (ret != PROTO_OK) {
                return -EINVAL;
            }
        }
        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            mesh_event_timing_set_local_first_slot_tx(&timing, true);
        }

        ret = mesh_append_event_timing_tlvs_at(outbound.payload,
                                               sizeof(outbound.payload),
                                               &payload_len,
                                               &timing,
                                               now_ms);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }
        encoded_delay_ms = uptime_ms_until_deadline(now_ms,
                                                    timing.next_event_time_ms);
    }
    ret = mesh_init_event_control(&outbound.packet,
                                  msg_type,
                                  DEVICE_ID,
                                  peer_id,
                                  nonzero_uptime_session_id(),
                                  mesh_next_event_control_seq(),
                                  (uint8_t)payload_len);
    if (ret != PROTO_OK) {
        return -EINVAL;
    }

    outbound.payload_len = (uint8_t)payload_len;
    outbound.next_hop_id = peer_id;
    outbound.radio_channel = UWB_CHANNEL_WAKE_CONTACT;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && !close_event) {
        status_debug_printf("DBG_EVENT_TX msg=%02x now=%u ref=%u next=%u cnt=%u txeven=%u\n",
                            msg_type,
                            now_ms,
                            timing_reference_ms,
                            timing.next_event_time_ms,
                            timing.event_counter,
                            timing.local_tx_on_even_events ? 1u : 0u);
    }
    ret = close_event ?
          mesh_send_outbound(&outbound, reason == NULL ? "mesh-event-control" : reason) :
          mesh_send_c5_control(&outbound,
                               C5_CONTACT_PURPOSE_CHANNEL9_TIMING_NEGOTIATION,
                               msg_type == MSG_MESH_EVENT_ACCEPT ?
                                   MESH_C5_CONTROL_ACCEPTED_EXCHANGE :
                                   MESH_C5_CONTROL_WAKE_IF_NEEDED,
                               reason == NULL ? "mesh-event-control" : reason);
    if (ret < 0) {
        return ret;
    }
    if (!close_event) {
        uint32_t tx_done_ms = k_uptime_get_32();

        mesh_event_timing_reanchor_after_control_tx(
            &timing,
            tx_done_ms,
            encoded_delay_ms,
            MESH_EVENT_CONTROL_CH5_AIRTIME_MS);
        if (transmitted_timing != NULL) {
            *transmitted_timing = timing;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_EVENT_TX_ALIGNED msg=%02x done=%u encoded=%u rxoff=%u next=%u\n",
                                msg_type,
                                tx_done_ms,
                                encoded_delay_ms,
                                MESH_EVENT_CONTROL_CH5_AIRTIME_MS,
                                timing.next_event_time_ms);
        }
    }
    if (install_local) {
        struct mesh_event_timing local_timing = timing;

        if (msg_type == MSG_MESH_EVENT_PROPOSE) {
            mesh_event_timing_set_local_first_slot_tx(&local_timing, true);
        }
        ret = mesh_install_channel9_timing(peer_id,
                                           &local_timing,
                                           reason == NULL ? "mesh-event-control" : reason);
        if (ret != PROTO_OK) {
            return mesh_errno_from_proto(ret);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_LOCAL_TIMING_INSTALLED\n");
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                      mesh_channel9_prepare_start_ms(&local_timing)));
    }
    return 0;
}

static void mesh_event_propose_clear(void)
{
    uint64_t peer_id = mesh_event_propose_retry.peer_id;

    app_mesh_event_retry_clear(&mesh_event_propose_retry);
    memset(&mesh_event_propose_record, 0, sizeof(mesh_event_propose_record));
    if (mesh_route_ready_event_peer_id == peer_id) {
        mesh_route_ready_event_peer_id = 0u;
    }
}

static void mesh_event_accept_rx_clear_peer(uint64_t peer_id)
{
    if (mesh_event_accept_rx_cache.active &&
        mesh_event_accept_rx_cache.peer_id == peer_id) {
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
    }
}

static int mesh_event_negotiation_schedule_next(void)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t delay_ms = UINT32_MAX;
    const struct app_mesh_event_retry_state *states[] = {
        &mesh_event_accept_retry.retry,
        &mesh_event_propose_retry,
    };

    for (size_t i = 0u; i < ARRAY_SIZE(states); i++) {
        const struct app_mesh_event_retry_state *state = states[i];
        uint32_t candidate_ms;

        if (!state->active) {
            continue;
        }
        if (app_mesh_event_retry_expired(state, now_ms)) {
            delay_ms = 0u;
            break;
        }
        candidate_ms = state->retry_due_ms != 0u ?
                       state->retry_due_ms : state->deadline_ms;
        candidate_ms = uptime_ms_until_deadline(now_ms, candidate_ms);
        if (candidate_ms < delay_ms) {
            delay_ms = candidate_ms;
        }
    }
    if (delay_ms == UINT32_MAX) {
        return mesh_cancel_delayable(&mesh_event_negotiation_retry_work);
    }
    return mesh_reschedule_delayable(&mesh_event_negotiation_retry_work,
                                     delay_ms);
}

static bool mesh_event_retry_after_failure(
    struct app_mesh_event_retry_state *state,
    enum app_mesh_rf_retry_policy policy,
    bool rf_started,
    const char *reason)
{
    uint32_t delay_ms = 0u;
    bool scheduled = app_mesh_event_retry_note_failure(
        state,
        policy,
        k_uptime_get_32(),
        sys_rand32_get(),
        rf_started,
        &delay_ms);

    if (scheduled) {
        int schedule_ret = mesh_event_negotiation_schedule_next();

        status_debug_printf("DBG_EVENT_RETRY_SCHED msg=%02x peer=0x%llx delay=%u rf=%u round=%u ret=%d reason=%s\n",
                            state->retry_key.message_type,
                            (unsigned long long)state->peer_id,
                            delay_ms,
                            rf_started ? 1u : 0u,
                            state->retry.retry_round,
                            schedule_ret,
                            reason == NULL ? "event-retry" : reason);
    }
    return scheduled;
}

static int mesh_propose_event_after_channel5_contact(uint64_t peer_id, const char *reason)
{
    bool require_accept = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                          DEVICE_ROLE != ROLE_GATEWAY;
    struct mesh_event_timing transmitted_timing = {0};
    bool rf_started = false;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (app_mesh_event_retry_expired(&mesh_event_propose_retry, now_ms)) {
        mesh_event_propose_clear();
    }
    if (!mesh_event_propose_retry.active) {
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;

        /* A completed ACCEPT belongs only to the proposal that created it. */
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
        ret = mesh_prepare_event_control_record(&mesh_event_propose_record,
                                                peer_id,
                                                MSG_MESH_EVENT_PROPOSE,
                                                NULL,
                                                0u,
                                                0u);
        if (ret < 0) {
            return ret;
        }
        request = mesh_event_request_identity(
            &mesh_event_propose_record.packet,
            mesh_event_propose_record.payload,
            mesh_event_propose_record.payload_len);
        retry_key = mesh_rf_retry_packet_key(
            &mesh_event_propose_record.packet,
            APP_MESH_RF_RETRY_OPERATION_EVENT_PROPOSE);
        ret = app_mesh_event_retry_begin(
            &mesh_event_propose_retry,
            peer_id,
            &request,
            &retry_key,
            now_ms,
            now_ms + MESH_EVENT_PROPOSE_RETRY_DEADLINE_MS,
            mesh_event_propose_record.timing.event_interval_ms,
            0u);
        if (ret < 0) {
            memset(&mesh_event_propose_record, 0,
                   sizeof(mesh_event_propose_record));
            return ret;
        }
    } else if (mesh_event_propose_retry.peer_id != peer_id ||
               !mesh_event_propose_record.valid) {
        return -EBUSY;
    } else if (mesh_event_propose_retry.retry_due_ms != 0u &&
               !app_mesh_event_retry_due(&mesh_event_propose_retry, now_ms)) {
        return -EAGAIN;
    }

    ret = mesh_send_event_control_record(
        &mesh_event_propose_record,
        MESH_C5_CONTROL_WAKE_IF_NEEDED,
        reason,
        &rf_started,
        &transmitted_timing);
    if (ret < 0) {
        LOG_WRN("mesh channel-9 event proposal failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-propose" : reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_PROPOSE_FAIL\n");
        }
        if (!mesh_event_retry_after_failure(
                &mesh_event_propose_retry,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                rf_started,
                "event-propose-send")) {
            mesh_event_propose_clear();
            return -ETIMEDOUT;
        }
        return ret;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_EVENT_PROPOSE_SENT\n");
    }

    if (require_accept) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_ACCEPT_LISTEN\n");
        }
        ret = mesh_listen_for_route_reply(peer_id,
                                          "event-accept",
                                          MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
                                          NULL,
                                          NULL);
        if (ret == 0 && mesh_event_propose_retry.active) {
            ret = -EPROTO;
        }
        if (ret < 0) {
            (void)mesh_ch9_ack_batch_clear_for_peer(peer_id, "event-accept-timeout");
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_EVENT_ACCEPT_TIMEOUT\n");
            }
            LOG_WRN("mesh channel-9 event ACCEPT not received: peer=0x%016llx ret=%d",
                    (unsigned long long)peer_id,
                    ret);
            if (!mesh_event_retry_after_failure(
                    &mesh_event_propose_retry,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    true,
                    "event-accept-missed")) {
                mesh_event_propose_clear();
                return -ETIMEDOUT;
            }
            return ret;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_ACCEPT_RX\n");
        }
    } else if (app_mesh_event_retry_claim_timing_install(
                   &mesh_event_propose_retry)) {
        ret = mesh_install_channel9_timing(peer_id,
                                           &transmitted_timing,
                                           reason == NULL ?
                                               "event-propose" : reason);
        if (ret != PROTO_OK) {
            mesh_event_propose_clear();
            return mesh_errno_from_proto(ret);
        }
    }
    mesh_event_propose_clear();
    return 0;
}

static bool mesh_packet_is_event_control_type(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static void mesh_event_accept_clear(bool rollback, const char *reason)
{
    uint64_t peer_id = mesh_event_accept_retry.retry.peer_id;

    if (rollback && mesh_event_accept_retry.retry.active &&
        !mesh_event_accept_retry.retry.timing_installed &&
        mesh_id_is_unicast(peer_id)) {
        if (mesh_event_accept_retry.had_previous_timing) {
            (void)mesh_install_channel9_timing(
                peer_id,
                &mesh_event_accept_retry.previous_timing,
                reason == NULL ? "event-accept-rollback" : reason);
        } else {
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
        }
    }
    memset(&mesh_event_accept_retry, 0, sizeof(mesh_event_accept_retry));
}

static void mesh_event_accept_completed_prune(uint32_t now_ms)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (entry->completion.valid &&
            uptime_deadline_reached(now_ms,
                                    entry->completion.expires_at_ms)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void mesh_event_accept_completed_clear_peer(uint64_t peer_id)
{
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (entry->completion.valid &&
            entry->completion.peer_id == peer_id) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void mesh_event_accept_completed_store(
    const struct mesh_event_control_record *response,
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint32_t expires_at_ms,
    uint16_t retry_round)
{
    struct mesh_event_accept_completed *selected = NULL;

    if (response == NULL || !response->valid || request == NULL) {
        return;
    }
    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];
        enum app_mesh_event_request_match match =
            app_mesh_event_completion_match(&entry->completion,
                                            peer_id,
                                            request,
                                            now_ms);

        if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
            selected = entry;
            break;
        }
        if (selected == NULL && !entry->completion.valid) {
            selected = entry;
        }
    }
    if (selected == NULL) {
        selected = &mesh_event_accept_completed[
            mesh_event_accept_completed_cursor %
            ARRAY_SIZE(mesh_event_accept_completed)];
        mesh_event_accept_completed_cursor++;
    }

    memset(selected, 0, sizeof(*selected));
    selected->response = *response;
    if (app_mesh_event_completion_store(&selected->completion,
                                        peer_id,
                                        request,
                                        now_ms,
                                        expires_at_ms) < 0) {
        memset(selected, 0, sizeof(*selected));
    } else {
        selected->retry_round = retry_round;
    }
}

static void mesh_event_accept_completed_note_retry_round(
    uint64_t peer_id,
    const struct app_mesh_event_request_identity *request,
    uint32_t now_ms,
    uint16_t retry_round)
{
    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        if (app_mesh_event_completion_match(&entry->completion,
                                            peer_id,
                                            request,
                                            now_ms) ==
            APP_MESH_EVENT_REQUEST_DUPLICATE) {
            entry->retry_round = retry_round;
            return;
        }
    }
}

static int mesh_event_accept_finish_send(
    const struct mesh_event_timing *transmitted_timing,
    const char *reason)
{
    struct app_mesh_event_retry_state *retry =
        &mesh_event_accept_retry.retry;
    uint64_t peer_id = retry->peer_id;
    uint16_t retry_round;
    int ret;

    if (transmitted_timing == NULL || !retry->active) {
        return -EINVAL;
    }
    if (app_mesh_event_retry_claim_timing_install(retry)) {
        if (!mesh_event_accept_retry.replay_existing_response &&
            !app_mesh_c5_event_accept_realign_is_reserved(
                &mesh_event_accept_retry.reservation_timing,
                transmitted_timing,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS)) {
            status_debug_printf("DBG_EVENT_ACCEPT_REALIGN_OUT_OF_BOUNDS prev=0x%llx reserved=%u actual=%u slop=%u\n",
                                (unsigned long long)peer_id,
                                mesh_event_accept_retry.reservation_timing.next_event_time_ms,
                                transmitted_timing->next_event_time_ms,
                                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS);
            retry->timing_installed = false;
            mesh_event_accept_clear(true, "event-accept-realign-rollback");
            return -ERANGE;
        }
        ret = mesh_install_channel9_timing_direction(
            peer_id,
            transmitted_timing,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
            reason == NULL ? "event-accept-tx-aligned" : reason);
        if (ret != PROTO_OK) {
            retry->timing_installed = false;
            mesh_event_accept_clear(true, "event-accept-install-rollback");
            return mesh_errno_from_proto(ret);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_EVENT_ACCEPT_REALIGNED prev=0x%llx next=%u\n",
                                (unsigned long long)peer_id,
                                transmitted_timing->next_event_time_ms);
        }
        mesh_schedule_uwb_rx(uptime_ms_until_deadline(
            k_uptime_get_32(),
            mesh_channel9_prepare_start_ms(transmitted_timing)));
        if (mesh_gateway_route_test_role()) {
            mesh_gateway_route_test_clear_preempt(peer_id,
                                                  "event-accept-installed");
        }
    }
    retry_round = retry->retry.retry_round;
    app_mesh_event_retry_note_send_success(retry);
    mesh_event_accept_completed_store(
        &mesh_event_accept_retry.response,
        peer_id,
        &retry->request,
        k_uptime_get_32(),
        retry->deadline_ms,
        retry_round);
    mesh_event_accept_clear(false, "event-accept-complete");
    (void)mesh_event_negotiation_schedule_next();
    return 0;
}

static int mesh_event_accept_attempt(const char *reason)
{
    struct mesh_event_timing transmitted_timing = {0};
    struct app_mesh_event_retry_state *retry =
        &mesh_event_accept_retry.retry;
    bool rf_started = false;
    int ret;

    if (!retry->active || !mesh_event_accept_retry.response.valid) {
        return -ENOENT;
    }
    atomic_set(&mesh_rx_response_active_state, 1);
    ret = mesh_send_event_control_record(
        &mesh_event_accept_retry.response,
        MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
        reason == NULL ? "mesh-event-accept" : reason,
        &rf_started,
        &transmitted_timing);
    atomic_set(&mesh_rx_response_active_state, 0);
    if (ret == 0) {
        return mesh_event_accept_finish_send(&transmitted_timing, reason);
    }

    LOG_WRN("mesh channel-9 event ACCEPT failed: next=0x%016llx ret=%d",
            (unsigned long long)retry->peer_id,
            ret);
    if (!mesh_event_retry_after_failure(
            retry,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            rf_started,
            "event-accept-send")) {
        mesh_event_accept_clear(true, "event-accept-deadline");
        return -ETIMEDOUT;
    }
    return ret;
}

static bool mesh_event_accept_duplicate(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct app_mesh_event_request_identity request =
        mesh_event_request_identity(packet, payload, payload_len);
    enum app_mesh_event_request_match match;
    uint32_t now_ms = k_uptime_get_32();

    if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry, now_ms)) {
        mesh_event_accept_clear(
            !mesh_event_accept_retry.retry.timing_installed,
            "event-accept-cache-expired");
    }
    match = app_mesh_event_retry_match(&mesh_event_accept_retry.retry,
                                       previous_hop_id,
                                       &request);
    if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
        if (mesh_event_accept_retry.retry.response_sent &&
            mesh_event_accept_retry.retry.retry_due_ms == 0u) {
            (void)mesh_event_retry_after_failure(
                &mesh_event_accept_retry.retry,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                true,
                "event-propose-duplicate");
        }
        status_debug_note("DBG_EVENT_PROPOSE_DUPLICATE\n");
        return true;
    }
    if (match == APP_MESH_EVENT_REQUEST_BUSY &&
        mesh_event_accept_retry.replay_existing_response) {
        /*
         * A completed response remains in the completion cache, so its
         * best-effort replay must never reserve the singleton ahead of new
         * work from another peer. The duplicate can recreate this replay on
         * its next proposal without losing response identity or retry round.
         */
        mesh_event_accept_completed_note_retry_round(
            mesh_event_accept_retry.retry.peer_id,
            &mesh_event_accept_retry.retry.request,
            now_ms,
            mesh_event_accept_retry.retry.retry.retry_round);
        mesh_event_accept_clear(false, "event-accept-replay-preempt");
        match = APP_MESH_EVENT_REQUEST_NEW;
    }
    if (match == APP_MESH_EVENT_REQUEST_CONFLICT ||
        match == APP_MESH_EVENT_REQUEST_BUSY) {
        status_debug_note(match == APP_MESH_EVENT_REQUEST_CONFLICT ?
                          "DBG_EVENT_PROPOSE_CONFLICT\n" :
                          "DBG_EVENT_PROPOSE_BUSY\n");
        return true;
    }

    mesh_event_accept_completed_prune(now_ms);
    for (size_t i = 0u; i < ARRAY_SIZE(mesh_event_accept_completed); i++) {
        struct mesh_event_accept_completed *entry =
            &mesh_event_accept_completed[i];

        match = app_mesh_event_completion_match(&entry->completion,
                                                previous_hop_id,
                                                &request,
                                                now_ms);
        if (match == APP_MESH_EVENT_REQUEST_CONFLICT) {
            status_debug_note("DBG_EVENT_PROPOSE_CONFLICT\n");
            return true;
        }
        if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
            struct mesh_event_timing active_timing = {0};
            struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &entry->response.packet,
                    APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
            bool had_active_timing = mesh_find_active_channel9_timing(
                previous_hop_id, now_ms, &active_timing);
            int ret;

            memset(&mesh_event_accept_retry, 0,
                   sizeof(mesh_event_accept_retry));
            mesh_event_accept_retry.response = entry->response;
            ret = app_mesh_event_retry_begin(
                &mesh_event_accept_retry.retry,
                previous_hop_id,
                &entry->completion.request,
                &retry_key,
                now_ms,
                entry->completion.expires_at_ms,
                entry->response.timing.event_interval_ms,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS / 2u);
            if (ret < 0) {
                mesh_event_accept_clear(false,
                                        "event-accept-replay-begin");
                return true;
            }
            mesh_event_accept_retry.previous_timing = active_timing;
            mesh_event_accept_retry.had_previous_timing =
                had_active_timing;
            mesh_event_accept_retry.replay_existing_response = true;
            if (app_mesh_event_retry_resume_backoff(
                    &mesh_event_accept_retry.retry,
                    entry->retry_round) < 0) {
                mesh_event_accept_clear(false,
                                        "event-accept-replay-state");
                return true;
            }
            if (!mesh_event_retry_after_failure(
                    &mesh_event_accept_retry.retry,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    false,
                    "event-propose-duplicate")) {
                mesh_event_accept_clear(false,
                                        "event-accept-replay-deadline");
            }
            status_debug_note("DBG_EVENT_PROPOSE_DUPLICATE\n");
            return true;
        }
    }
    return false;
}

static enum app_mesh_event_request_match mesh_event_accept_rx_match(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id)
{
    struct app_mesh_event_request_identity request =
        mesh_event_request_identity(packet, payload, payload_len);
    enum app_mesh_event_request_match match;

    if (app_mesh_event_retry_expired(&mesh_event_accept_rx_cache,
                                     k_uptime_get_32())) {
        app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
    }
    match = app_mesh_event_retry_match(&mesh_event_accept_rx_cache,
                                       previous_hop_id,
                                       &request);
    if (match == APP_MESH_EVENT_REQUEST_DUPLICATE) {
        status_debug_note("DBG_EVENT_ACCEPT_DUPLICATE\n");
        return match;
    }
    if (match == APP_MESH_EVENT_REQUEST_CONFLICT ||
        match == APP_MESH_EVENT_REQUEST_BUSY) {
        status_debug_note("DBG_EVENT_ACCEPT_UNEXPECTED\n");
        return match;
    }
    return APP_MESH_EVENT_REQUEST_NEW;
}

static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t received_at_ms)
{
    struct mesh_event_timing timing = {0};
    bool replayed_event_accept = false;
    uint32_t timing_reference_ms;
    int ret;

    if (packet == NULL) {
        return false;
    }

    if (!mesh_packet_is_event_control_type(packet->msg_type)) {
        return false;
    }
    if (!mesh_id_is_unicast(previous_hop_id)) {
        LOG_WRN("mesh event control ignored without unicast previous hop");
        return true;
    }
    if (packet->msg_type == MSG_MESH_EVENT_END) {
        (void)mesh_ch9_ack_batch_clear_for_peer(previous_hop_id, "event-end-rx");
        mesh_event_accept_rx_clear_peer(previous_hop_id);
        mesh_event_accept_completed_clear_peer(previous_hop_id);
        mesh_relay_clear_channel9_timing(&mesh_runtime, previous_hop_id);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_EVENT_END_RX\n");
        }
        mesh_restore_anchor_low_duty_if_no_ch9("event-end-rx");
        return true;
    }

    timing_reference_ms = mesh_event_control_rx_reference_ms(received_at_ms);
    ret = mesh_event_timing_from_tlvs_at(&timing,
                                         payload,
                                         payload_len,
                                         timing_reference_ms,
                                         true);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing parse failed: msg=0x%02x ret=%d",
                packet->msg_type,
                ret);
        return true;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_EVENT_RX msg=%02x recv=%u ref=%u next=%u cnt=%u txeven=%u\n",
                            packet->msg_type,
                            received_at_ms,
                            timing_reference_ms,
                            timing.next_event_time_ms,
                            timing.event_counter,
                            timing.local_tx_on_even_events ? 1u : 0u);
    }
    if (packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        struct mesh_event_timing active_timing = {0};
        struct mesh_event_timing reservation_timing = {0};
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;
        uint32_t now_ms = k_uptime_get_32();
        bool had_active_timing;

        if (mesh_event_accept_duplicate(packet,
                                        payload,
                                        payload_len,
                                        previous_hop_id)) {
            return true;
        }
        mesh_event_timing_set_local_first_slot_tx(&timing, false);
        had_active_timing = mesh_find_active_channel9_timing(previous_hop_id,
                                                             now_ms,
                                                             &active_timing);
        if (had_active_timing) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_EVENT_PROPOSE_REPLACE_ACTIVE now=%u old=%u new=%u old_cnt=%u new_cnt=%u\n",
                                    now_ms,
                                    active_timing.next_event_time_ms,
                                    timing.next_event_time_ms,
                                    active_timing.event_counter,
                                    timing.event_counter);
            }
        }
        if (mesh_gateway_route_test_slots_full_for(previous_hop_id)) {
            status_debug_note("DBG_GATEWAY_EVENT_REJECT_FULL\n");
            status_debug_printf("DBG_GATEWAY_EVENT_REJECT prev=0x%llx active=%u\n",
                                (unsigned long long)previous_hop_id,
                                mesh_channel9_connection_count());
            mesh_gateway_route_test_clear_preempt(previous_hop_id,
                                                  "event-propose-full");
            return true;
        }
        if (!app_mesh_c5_event_accept_reservation(
                &timing,
                MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS,
                &reservation_timing)) {
            status_debug_note("DBG_EVENT_PROPOSE_REJECT_RESERVATION\n");
            return true;
        }
        ret = mesh_install_channel9_timing_direction(
            previous_hop_id,
            &reservation_timing,
            MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM,
            "event-propose-reserve");
        if (ret != PROTO_OK) {
            status_debug_printf("DBG_EVENT_PROPOSE_REJECT_INSTALL prev=0x%llx ret=%d\n",
                                (unsigned long long)previous_hop_id,
                                ret);
            LOG_WRN("mesh channel-9 event proposal rejected before ACCEPT: next=0x%016llx ret=%d",
                    (unsigned long long)previous_hop_id,
                    ret);
            return true;
        }
        ret = mesh_prepare_event_control_record(
            &mesh_event_accept_retry.response,
            previous_hop_id,
            MSG_MESH_EVENT_ACCEPT,
            &timing,
            packet->session_id,
            packet->seq);
        if (ret < 0) {
            if (had_active_timing) {
                (void)mesh_install_channel9_timing(previous_hop_id,
                                                   &active_timing,
                                                   "event-accept-prepare-rollback");
            } else {
                mesh_relay_clear_channel9_timing(&mesh_runtime,
                                                 previous_hop_id);
            }
            return true;
        }
        request = mesh_event_request_identity(packet, payload, payload_len);
        retry_key = mesh_rf_retry_packet_key(
            &mesh_event_accept_retry.response.packet,
            APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
        ret = app_mesh_event_retry_begin(
            &mesh_event_accept_retry.retry,
            previous_hop_id,
            &request,
            &retry_key,
            now_ms,
            now_ms + MESH_EVENT_ACCEPT_RETRY_DEADLINE_MS,
            timing.event_interval_ms,
            MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS / 2u);
        if (ret < 0) {
            if (had_active_timing) {
                (void)mesh_install_channel9_timing(previous_hop_id,
                                                   &active_timing,
                                                   "event-accept-state-rollback");
            } else {
                mesh_relay_clear_channel9_timing(&mesh_runtime,
                                                 previous_hop_id);
            }
            memset(&mesh_event_accept_retry, 0,
                   sizeof(mesh_event_accept_retry));
            return true;
        }
        mesh_event_accept_retry.reservation_timing = reservation_timing;
        mesh_event_accept_retry.previous_timing = active_timing;
        mesh_event_accept_retry.had_previous_timing = had_active_timing;
        atomic_set(&mesh_rx_response_active_state, 1);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
        }
        atomic_set(&mesh_rx_response_active_state, 0);
        (void)mesh_event_accept_attempt("mesh-event-accept");
        return true;
    } else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        struct app_mesh_event_request_identity request;
        struct app_mesh_rf_retry_key retry_key;
        enum app_mesh_event_accept_correlation correlation;
        enum app_mesh_event_request_match accept_match;
        uint32_t now_ms = k_uptime_get_32();

        accept_match = mesh_event_accept_rx_match(packet,
                                                  payload,
                                                  payload_len,
                                                  previous_hop_id);
        if (accept_match == APP_MESH_EVENT_REQUEST_CONFLICT ||
            accept_match == APP_MESH_EVENT_REQUEST_BUSY) {
            return true;
        }
        replayed_event_accept =
            accept_match == APP_MESH_EVENT_REQUEST_DUPLICATE;
        if (!replayed_event_accept) {
            correlation = app_mesh_event_accept_classify(
                &mesh_event_propose_retry,
                packet->src_id,
                packet->dst_id,
                previous_hop_id,
                packet->session_id,
                packet->seq,
                mesh_event_accept_timing_compatible(
                    &timing, &mesh_event_propose_record));
            if (correlation == APP_MESH_EVENT_ACCEPT_REJECT) {
                status_debug_note("DBG_EVENT_ACCEPT_UNCORRELATED\n");
                return true;
            }
            if (correlation == APP_MESH_EVENT_ACCEPT_LEGACY) {
                status_debug_note("DBG_EVENT_ACCEPT_LEGACY_IDENTITY\n");
            }
            request = mesh_event_request_identity(packet, payload, payload_len);
            retry_key = mesh_rf_retry_packet_key(
                packet, APP_MESH_RF_RETRY_OPERATION_EVENT_ACCEPT);
            ret = app_mesh_event_retry_begin(
                &mesh_event_accept_rx_cache,
                previous_hop_id,
                &request,
                &retry_key,
                now_ms,
                now_ms + MESH_EVENT_DEFAULT_SUPERVISION_MS,
                timing.event_interval_ms,
                0u);
            if (ret < 0 ||
                !app_mesh_event_retry_claim_timing_install(
                    &mesh_event_accept_rx_cache)) {
                app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
                return true;
            }
        }
        /* Exact replay bytes carry a relative delay, so install them again. */
        mesh_event_timing_set_local_first_slot_tx(&timing, true);
    }

    if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        ret = mesh_install_channel9_timing_direction(
            previous_hop_id,
            &timing,
            MESH_RELAY_CHANNEL9_DIRECTION_UPSTREAM,
            "event-control-rx");
    } else {
        ret = mesh_install_channel9_timing(previous_hop_id,
                                           &timing,
                                           "event-control-rx");
    }
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing install failed: next=0x%016llx ret=%d",
                (unsigned long long)previous_hop_id,
                ret);
        if (packet->msg_type == MSG_MESH_EVENT_ACCEPT &&
            !replayed_event_accept) {
            app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
        }
        return true;
    }

    if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        if (!replayed_event_accept) {
            app_mesh_event_retry_note_send_success(&mesh_event_accept_rx_cache);
            mesh_event_propose_clear();
        }
    }

    mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                  mesh_channel9_prepare_start_ms(&timing)));
    if (mesh_gateway_route_test_role() &&
        packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        mesh_gateway_route_test_clear_preempt(previous_hop_id,
                                              "event-accept-installed");
    }
    return true;
}

static void mesh_event_negotiation_retry_work_handler(struct k_work *work)
{
    uint32_t now_ms = k_uptime_get_32();

    ARG_UNUSED(work);

    if (app_mesh_event_retry_expired(&mesh_event_accept_retry.retry, now_ms)) {
        mesh_event_accept_clear(
            !mesh_event_accept_retry.retry.timing_installed,
            "event-accept-retry-expired");
    } else if (app_mesh_event_retry_due(&mesh_event_accept_retry.retry,
                                        now_ms)) {
        (void)mesh_event_accept_attempt("mesh-event-accept-retry");
    }

    now_ms = k_uptime_get_32();
    if (app_mesh_event_retry_expired(&mesh_event_propose_retry, now_ms)) {
        mesh_event_propose_clear();
        if (mesh_route_waiting_tx_valid) {
            mesh_schedule_route_waiting_retry_after(
                "event-propose-expired", 1u);
        }
    } else if (app_mesh_event_retry_due(&mesh_event_propose_retry, now_ms)) {
        uint64_t peer_id = mesh_event_propose_retry.peer_id;
        int ret = mesh_propose_event_after_channel5_contact(
            peer_id, "mesh-event-propose-retry");

        if (ret == 0) {
            if (mesh_route_waiting_tx_valid) {
                mesh_schedule_route_waiting_retry_after(
                    "event-propose-complete", 1u);
            }
            if (mesh_relay_tx_active(&mesh_runtime)) {
                (void)mesh_relay_defer_pending_retry(
                    &mesh_runtime, k_uptime_get_32() + 1u);
                mesh_schedule_tx_timeout();
            }
        }
    }
    (void)mesh_event_negotiation_schedule_next();
}

static void mesh_close_channel9_connection(uint64_t peer_id, const char *reason)
{
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !mesh_id_is_unicast(peer_id) ||
        peer_id == DEVICE_ID) {
        return;
    }

    ret = mesh_send_event_control(peer_id,
                                  MSG_MESH_EVENT_END,
                                  NULL,
                                  false,
                                  reason == NULL ? "mesh-event-end" : reason,
                                  NULL);
    if (ret == 0) {
        status_debug_note("DBG_CH9_EVENT_END_TX\n");
    } else {
        status_debug_note("DBG_CH9_EVENT_END_FAIL\n");
        LOG_WRN("mesh channel-9 event close TX failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-end" : reason);
    }

    (void)mesh_ch9_ack_batch_clear_for_peer(peer_id,
                                            reason == NULL ? "event-end-tx" : reason);
    mesh_event_accept_rx_clear_peer(peer_id);
    mesh_event_accept_completed_clear_peer(peer_id);
    mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
    mesh_restore_anchor_low_duty_if_no_ch9("event-end-tx");
}

static uint8_t mesh_capture_downstream_channel9_peers(
    uint64_t *peer_ids,
    uint8_t capacity)
{
    uint8_t count = 0u;

    if (peer_ids == NULL || capacity == 0u) {
        return 0u;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS && count < capacity; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (entry->valid &&
            entry->direction == MESH_RELAY_CHANNEL9_DIRECTION_DOWNSTREAM) {
            peer_ids[count++] = entry->next_hop_id;
        }
    }
    return count;
}

static bool mesh_channel9_peer_timing_exists(uint64_t peer_id)
{
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        if (mesh_runtime.event_timings[i].valid &&
            mesh_runtime.event_timings[i].next_hop_id == peer_id) {
            return true;
        }
    }
    return false;
}

static void mesh_notify_removed_downstream_peers(const uint64_t *peer_ids,
                                                 uint8_t count,
                                                 const char *reason)
{
    if (peer_ids == NULL) {
        return;
    }
    for (uint8_t i = 0u; i < count; i++) {
        if (!mesh_channel9_peer_timing_exists(peer_ids[i])) {
            mesh_close_channel9_connection(peer_ids[i], reason);
        }
    }
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_packet_is_local_origin_priority(const struct proto_packet *packet)
{
    return packet != NULL &&
           packet->src_id == DEVICE_ID &&
           (packet->msg_type == MSG_CLICK_REPORT ||
            packet->msg_type == MSG_COMMAND_RESULT);
}

static bool mesh_outbound_is_local_origin_priority(const struct mesh_outbound *out)
{
    return out != NULL && mesh_packet_is_local_origin_priority(&out->packet);
}
#endif

static bool mesh_runtime_has_transit_reservation(void)
{
    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *downlink = &mesh_runtime.downlinks[i];

        if (!downlink->valid) {
            continue;
        }
        for (uint8_t j = 0u; j < MESH_RELAY_EVENT_TIMINGS; j++) {
            if (mesh_runtime.event_timings[j].valid &&
                mesh_runtime.event_timings[j].next_hop_id ==
                    downlink->next_hop_id) {
                return true;
            }
        }
    }
    return false;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static uint8_t mesh_collect_active_transit_peers(uint64_t *peers, uint8_t peer_cap)
{
    uint8_t peer_count = 0u;

    if (peers == NULL || peer_cap == 0u) {
        return 0u;
    }

    for (uint8_t i = 0u; i < MESH_RELAY_DOWNLINK_ROUTES; i++) {
        const struct mesh_downlink_entry *downlink = &mesh_runtime.downlinks[i];
        uint64_t peer_id = downlink->next_hop_id;
        bool duplicate = false;
        bool timing_valid = false;

        if (!downlink->valid ||
            !mesh_id_is_unicast(peer_id) ||
            peer_id == DEVICE_ID) {
            continue;
        }
        for (uint8_t j = 0u; j < MESH_RELAY_EVENT_TIMINGS; j++) {
            if (mesh_runtime.event_timings[j].valid &&
                mesh_runtime.event_timings[j].next_hop_id == peer_id) {
                timing_valid = true;
                break;
            }
        }
        if (!timing_valid) {
            continue;
        }
        for (uint8_t j = 0u; j < peer_count; j++) {
            if (peers[j] == peer_id) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && peer_count < peer_cap) {
            peers[peer_count++] = peer_id;
        }
    }
    return peer_count;
}

static bool mesh_ch9_tx_pending_has_local_origin_priority(void)
{
    if (!mesh_ch9_tx_pending.active) {
        return false;
    }

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        if (mesh_outbound_is_local_origin_priority(
                &mesh_ch9_tx_pending.entries[i].outbound)) {
            return true;
        }
    }
    return false;
}

static void mesh_reclaim_for_local_origin_priority(const struct mesh_outbound *out,
                                                   const char *reason,
                                                   bool send_notice)
{
    uint64_t peers[MESH_RELAY_EVENT_TIMINGS];
    uint8_t peer_count;
    bool cancelled_tx = false;
    bool abandoned_route = false;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !mesh_outbound_is_local_origin_priority(out)) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime) &&
        !mesh_packet_is_local_origin_priority(&mesh_runtime.pending.packet)) {
        mesh_relay_cancel_tx(&mesh_runtime);
        app_mesh_persistence_clear_outbox();
        (void)k_work_cancel_delayable(&mesh_tx_timeout_work);
        cancelled_tx = true;
    }

    if (mesh_route_waiting_tx_valid &&
        !mesh_outbound_is_local_origin_priority(&mesh_route_waiting_tx)) {
        mesh_route_waiting_tx_valid = false;
    }

    if (mesh_ch9_tx_pending.active &&
        !mesh_ch9_tx_pending_has_local_origin_priority()) {
        mesh_ch9_tx_pending_clear();
    }

    peer_count = mesh_collect_active_transit_peers(peers,
                                                  (uint8_t)ARRAY_SIZE(peers));
    if (mesh_runtime_has_transit_reservation()) {
        if (send_notice && !anchor_uwb_window_active()) {
            for (uint8_t i = 0u; i < peer_count; i++) {
                mesh_close_channel9_connection(peers[i], reason);
            }
        } else {
            for (uint8_t i = 0u; i < peer_count; i++) {
                (void)mesh_ch9_ack_batch_clear_for_peer(
                    peers[i],
                    reason == NULL ? "local-origin-priority" : reason);
            }
        }
        mesh_relay_abandon_transit_reservations(&mesh_runtime);
        mesh_restore_anchor_low_duty_if_no_ch9(
            reason == NULL ? "local-origin-priority" : reason);
        abandoned_route = true;
    }

    if (cancelled_tx || abandoned_route) {
        high_debug_log_event(
            "MESH_LOCAL_PRIORITY",
            "msg=0x%02x seq=%u cancelled_tx=%u abandoned_route=%u peers=%u reason=%s",
            out->packet.msg_type,
            out->packet.seq,
            cancelled_tx ? 1u : 0u,
            abandoned_route ? 1u : 0u,
            peer_count,
            reason == NULL ? "local-origin-priority" : reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_LOCAL_PRIORITY msg=0x%02x seq=%u cancel=%u abandon=%u peers=%u\n",
                                out->packet.msg_type,
                                out->packet.seq,
                                cancelled_tx ? 1u : 0u,
                                abandoned_route ? 1u : 0u,
                                peer_count);
        }
    }
}
#endif

static bool mesh_tx_can_wait_for_route(const struct mesh_outbound *out)
{
    if (out == NULL ||
        out->packet.dst_id == MESH_BROADCAST_ID ||
        out->packet.dst_id == DEVICE_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_GATEWAY_ACK:
    case MSG_MESH_DATA:
    case MSG_SELF_TEST_REPORT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out)
{
    if (out == NULL) {
        return 0u;
    }

    return UWB_MESH_FRAME_HEADER_LEN +
           proto_packet_encoded_len(out->packet.payload_len) +
           UWB_FRAME_CRC_LEN;
}

static uint32_t mesh_ch9_estimated_airtime_ms(size_t frame_len)
{
    const uint64_t frame_bits = ((uint64_t)frame_len + UWB_PHY_FCS_LEN) * 8ull;
    const uint64_t payload_us =
        ((frame_bits * 1000000ull) + MESH_CH9_DATA_RATE_BPS - 1ull) /
        MESH_CH9_DATA_RATE_BPS;
    const uint64_t total_us = payload_us + MESH_CH9_PHY_OVERHEAD_US;

    return (uint32_t)((total_us + 999ull) / 1000ull);
}

static uint32_t mesh_ch9_estimated_tx_ms(size_t frame_len)
{
    return mesh_ch9_estimated_airtime_ms(frame_len) + MESH_CH9_TX_FRAME_GAP_MS;
}

static uint32_t mesh_ch9_tx_offset_ms(const struct mesh_outbound *out)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || out == NULL) {
        return 0u;
    }
    if (out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return 0u;
    }

    return MESH_ROUTE_TEST_CH9_TX_OFFSET_MS;
}

static uint32_t mesh_ch9_slot_send_start_ms(const struct mesh_outbound *out,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms)
{
    uint32_t start_ms;

    if (plan == NULL) {
        return now_ms;
    }

    start_ms = plan->start_ms + mesh_ch9_tx_offset_ms(out);
    if (!uptime_deadline_reached(now_ms, start_ms)) {
        return start_ms;
    }
    return now_ms;
}

static uint32_t mesh_ch9_effective_send_start_ms(const struct mesh_outbound *out,
                                                 const struct mesh_event_plan *plan,
                                                 uint32_t now_ms)
{
    uint32_t send_start_ms = mesh_ch9_slot_send_start_ms(out, plan, now_ms);
    uint32_t config_ready_ms = now_ms + MESH_CH9_TX_CONFIG_GUARD_MS;

    return uptime_deadline_reached(send_start_ms, config_ready_ms) ?
           send_start_ms : config_ready_ms;
}

static bool mesh_ch9_tx_fits_plan(const struct mesh_outbound *out,
                                  const struct mesh_event_plan *plan,
                                  uint32_t now_ms,
                                  uint32_t *required_ms)
{
    uint32_t needed_ms;
    uint32_t available_ms;
    uint32_t effective_send_start_ms;
    uint32_t wait_ms;

    if (out == NULL || plan == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return true;
    }

    needed_ms = mesh_ch9_estimated_tx_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    effective_send_start_ms = mesh_ch9_effective_send_start_ms(out, plan, now_ms);
    wait_ms = uptime_deadline_reached(now_ms, effective_send_start_ms) ?
              0u : effective_send_start_ms - now_ms;
    if (uptime_deadline_reached(effective_send_start_ms, plan->end_ms)) {
        if (required_ms != NULL) {
            *required_ms = wait_ms + needed_ms;
        }
        return false;
    }
    available_ms = uptime_ms_until_deadline(effective_send_start_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = wait_ms + needed_ms;
    }
    return available_ms >= needed_ms;
}

int mesh_prepare_channel9_outbound(struct mesh_outbound *out,
                                   const struct mesh_event_plan *plan,
                                   uint32_t now_ms,
                                   uint32_t *required_ms)
{
    if (out == NULL || plan == NULL) {
        return -EINVAL;
    }

    out->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    out->earliest_tx_ms = mesh_ch9_slot_send_start_ms(out, plan, now_ms);
    if (!mesh_ch9_tx_fits_plan(out, plan, now_ms, required_ms)) {
        return -EBUSY;
    }
    return 0;
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_ch9_tx_fits_configured_slot(const struct mesh_outbound *out,
                                             const struct mesh_event_plan *plan,
                                             uint32_t now_ms,
                                             uint32_t send_start_ms,
                                             uint32_t *required_ms)
{
    uint32_t needed_ms;
    uint32_t available_ms;
    uint32_t wait_ms;

    if (out == NULL || plan == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return true;
    }

    wait_ms = uptime_deadline_reached(now_ms, send_start_ms) ?
              0u : send_start_ms - now_ms;
    needed_ms = wait_ms +
                mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    if (uptime_deadline_reached(now_ms, plan->end_ms) ||
        uptime_deadline_reached(send_start_ms, plan->end_ms)) {
        if (required_ms != NULL) {
            *required_ms = needed_ms;
        }
        return false;
    }
    available_ms = uptime_ms_until_deadline(now_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = needed_ms;
    }
    return available_ms >= needed_ms;
}
#endif

static void mesh_wait_until_ms(uint32_t target_ms)
{
    while (!uptime_deadline_reached(k_uptime_get_32(), target_ms)) {
        uint32_t delay_ms = uptime_ms_until_deadline(k_uptime_get_32(), target_ms);

        k_msleep(delay_ms > 5u ? 5u : delay_ms);
    }
}

static void mesh_store_route_waiting_tx(const struct mesh_outbound *out)
{
    struct mesh_outbound waiting;
    struct app_mesh_paused_delivery_store_result store_result;

    if (!mesh_tx_can_wait_for_route(out)) {
        return;
    }

    waiting = *out;
    if (waiting.queued_at_ms == 0u) {
        waiting.queued_at_ms = k_uptime_get_32();
    }
    app_mesh_paused_delivery_note_store(&mesh_paused_delivery,
                                        mesh_route_waiting_tx_valid,
                                        &mesh_route_waiting_tx,
                                        &waiting,
                                        &store_result);
    if (store_result.replaced_existing) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        high_debug_log_event("MESH_PAUSED_DELIVERY",
                             "phase=replace old_msg=0x%02x old_dst=0x%016llx old_seq=%u new_msg=0x%02x new_dst=0x%016llx new_seq=%u lost=%u",
                             mesh_route_waiting_tx.packet.msg_type,
                             (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                             mesh_route_waiting_tx.packet.seq,
                             waiting.packet.msg_type,
                             (unsigned long long)waiting.packet.dst_id,
                             waiting.packet.seq,
                             store_result.lost_count);
        LOG_WRN("mesh paused delivery replaced older packet: old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u",
                mesh_route_waiting_tx.packet.msg_type,
                mesh_route_waiting_tx.packet.seq,
                waiting.packet.msg_type,
                waiting.packet.seq,
                store_result.lost_count);
    }
    mesh_attach_paused_delivery_loss(&waiting, "route-wait-store");
    mesh_route_waiting_tx = waiting;
    mesh_route_waiting_tx_valid = true;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_STORE msg=0x%02x dst=0x%llx next=0x%llx seq=%u ch=%u queued=%u\n",
                            waiting.packet.msg_type,
                            (unsigned long long)waiting.packet.dst_id,
                            (unsigned long long)waiting.next_hop_id,
                            waiting.packet.seq,
                            waiting.radio_channel,
                            waiting.queued_at_ms);
    }
    mesh_schedule_route_waiting_retry_after("route-wait-store", 0u);
}

static void mesh_drop_route_waiting_tx(const char *reason)
{
    struct app_mesh_paused_delivery_store_result drop_result;

    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);

    high_debug_log_event("MESH_ROUTE_WAIT_DROP",
                         "reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u lost=%u",
                         reason == NULL ? "drop" : reason,
                         mesh_route_waiting_tx.packet.msg_type,
                         (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                         mesh_route_waiting_tx.packet.seq,
                         mesh_runtime.route_discovery.attempts,
                         drop_result.lost_count);
    LOG_WRN("mesh route waiting packet dropped: reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u lost=%u",
            reason == NULL ? "drop" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            mesh_runtime.route_discovery.attempts,
            drop_result.lost_count);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_DROP_LOST lost=%u reason=%s\n",
                            drop_result.lost_count,
                            reason == NULL ? "drop" : reason);
    }
    mesh_route_reply_handoff_clear("route-wait-drop");
    mesh_route_waiting_tx_valid = false;
}

void mesh_clear_route_waiting_tx(const struct proto_packet *packet)
{
    if (!mesh_route_waiting_tx_valid || packet == NULL) {
        return;
    }
    if (mesh_route_waiting_tx.packet.dst_id == packet->dst_id &&
        mesh_route_waiting_tx.packet.session_id == packet->session_id &&
        mesh_route_waiting_tx.packet.seq == packet->seq) {
        mesh_route_reply_handoff_clear("route-wait-clear");
        mesh_route_waiting_tx_valid = false;
    }
}

bool mesh_route_waiting_tx_active(void)
{
    return mesh_route_waiting_tx_valid;
}

static int mesh_handle_direct_gateway_retry_policy(const struct mesh_outbound *tx,
                                                   const char *reason,
                                                   int tx_ret)
{
    const struct route_candidate *selected;
    enum route_delivery_action action;
    uint64_t next_hop_id = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t random_value = sys_rand32_get();
    uint32_t delay_ms = 0u;
    uint32_t base_delay_ms = 0u;
    uint8_t failure_count = 0u;
    int select_ret = PROTO_ERR_NOT_FOUND;
    int route_ret;

    if (tx == NULL || tx->packet.dst_id != GATEWAY_ID) {
        return tx_ret;
    }

    action = route_record_failure_at(&mesh_runtime.upstream,
                                     ROUTE_FAILURE_GATEWAY_ACK,
                                     now_ms);
    if (action != ROUTE_DELIVERY_DISCOVER) {
        select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                tx->packet.dst_id,
                                                &next_hop_id);
    }
    if (action != ROUTE_DELIVERY_DISCOVER && select_ret == PROTO_OK) {
        if (action == ROUTE_DELIVERY_RETRY_CURRENT) {
            selected = route_selected(&mesh_runtime.upstream);
            failure_count = selected == NULL ? 1u : selected->failure_count;
            base_delay_ms = route_retry_backoff_ms(failure_count);
            delay_ms = mesh_relay_retry_backoff_ms(failure_count,
                                                   random_value);
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        app_mesh_persistence_clear_outbox();
        report_tx_set_retry_delay_override(delay_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_DIRECT_GW_ROUTE_RETRY action=%s seq=%u ret=%d fail=%u base=%u rand=%u delay=%u now=%u next=%u current=0x%llx selected=0x%llx reason=%s\n",
                                route_delivery_action_name(action),
                                tx->packet.seq,
                                tx_ret,
                                failure_count,
                                base_delay_ms,
                                random_value,
                                delay_ms,
                                now_ms,
                                now_ms + delay_ms,
                                (unsigned long long)tx->next_hop_id,
                                (unsigned long long)next_hop_id,
                                reason == NULL ? "direct-gateway" : reason);
        }
        return -EAGAIN;
    }

    mesh_relay_cancel_tx(&mesh_runtime);
    app_mesh_persistence_clear_outbox();
    mesh_store_route_waiting_tx(tx);
    mesh_direct_gateway_bulk_probe_suppress(now_ms, reason);
    route_ret = mesh_request_route(tx->packet.dst_id, reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_DIRECT_GW_ROUTE_REPAIR action=%s seq=%u ret=%d route_ret=%d now=%u current=0x%llx reason=%s\n",
                            route_delivery_action_name(ROUTE_DELIVERY_DISCOVER),
                            tx->packet.seq,
                            tx_ret,
                            route_ret,
                            now_ms,
                            (unsigned long long)tx->next_hop_id,
                            reason == NULL ? "direct-gateway" : reason);
    }
    if (route_ret == -ETIMEDOUT) {
        mesh_schedule_route_waiting_exhausted_retry("direct-gateway-route-exhausted");
    }
    return -EHOSTUNREACH;
}

static int mesh_try_deferred_gateway_ack_on_channel9(
    const struct mesh_outbound *pending,
    const char *reason,
    bool *send_attempted,
    bool *policy_deferred,
    uint32_t *wait_retry_delay_ms)
{
    struct mesh_outbound *ack = &mesh_deferred_gateway_ack_scratch;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    int ret;

    if (send_attempted != NULL) {
        *send_attempted = false;
    }
    if (policy_deferred != NULL) {
        *policy_deferred = false;
    }
    if (wait_retry_delay_ms != NULL) {
        *wait_retry_delay_ms = 0u;
    }
    if (pending == NULL ||
        pending->packet.msg_type != MSG_GATEWAY_ACK ||
        !mesh_id_is_unicast(pending->next_hop_id)) {
        return -EINVAL;
    }

    *ack = *pending;
    mesh_fill_channel5_requirements(&requirements);
    now_ms = k_uptime_get_32();
    (void)mesh_expire_channel9_timings(now_ms, reason);
    ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                               ack->next_hop_id,
                                               &requirements,
                                               now_ms,
                                               &plan);
    if (ret != PROTO_ERR_STALE) {
        mesh_event_note_plan_action(&mesh_event_stats, plan.action);
    }
    mesh_debug_channel5_preemption("gateway-ack",
                                   reason,
                                   ack->next_hop_id,
                                   &requirements,
                                   &plan,
                                   now_ms);
    if (ret == PROTO_ERR_STALE || ret == PROTO_ERR_NOT_FOUND) {
        return -EHOSTUNREACH;
    }
    if (ret == PROTO_ERR_BUSY) {
        if (mesh_event_plan_is_policy_deferral(plan.action)) {
            mesh_relay_note_channel9_missed(&mesh_runtime,
                                            ack->next_hop_id,
                                            &mesh_event_stats);
            if (policy_deferred != NULL) {
                *policy_deferred = true;
            }
        } else if (plan.action == MESH_EVENT_PLAN_WAIT &&
                   wait_retry_delay_ms != NULL) {
            (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                now_ms,
                plan.start_ms,
                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                wait_retry_delay_ms);
        }
        return -EBUSY;
    }
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    ack->radio_channel = MESH_EVENT_CHANNEL;
    if (send_attempted != NULL) {
        *send_attempted = true;
    }
    ret = mesh_send_outbound(ack, reason);
    if (ret == 0) {
        mesh_relay_note_tx_sent(&mesh_runtime, ack, k_uptime_get_32());
        mesh_note_channel9_local_tx(ack->next_hop_id, plan.start_ms);
    }
    return ret;
}

static void mesh_schedule_route_ready_event_retry(
    const struct app_mesh_route_ready_handoff_result *result,
    const char *reason)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t retry_delay_ms;
    int defer_ret = PROTO_OK;

    if (result == NULL || !result->schedule_event_accept_wait ||
        !mesh_id_is_unicast(result->peer_id)) {
        return;
    }

    if (result->remember_deferred_peer) {
        mesh_route_ready_event_peer_id = result->peer_id;
    }
    retry_delay_ms = mesh_event_propose_retry.active &&
                     mesh_event_propose_retry.retry_due_ms != 0u ?
        uptime_ms_until_deadline(now_ms,
                                 mesh_event_propose_retry.retry_due_ms) :
        25u + (sys_rand32_get() % 51u);
    if (mesh_route_waiting_tx_valid) {
        mesh_schedule_route_waiting_retry_after(
            reason,
            retry_delay_ms);
        return;
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }

    defer_ret = mesh_relay_defer_pending_retry(
        &mesh_runtime,
        now_ms + retry_delay_ms);
    status_debug_printf("DBG_ROUTE_READY_EVENT_RETRY peer=0x%llx ret=%d delay=%u state=%u\n",
                        (unsigned long long)result->peer_id,
                        defer_ret,
                        retry_delay_ms,
                        mesh_runtime.pending.state);
    mesh_schedule_tx_timeout();
}

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound *pending = &mesh_route_waiting_tx_scratch;
    struct app_mesh_tx_handoff_result handoff_result;
    struct app_mesh_route_ready_handoff_result route_ready_result;
    struct app_mesh_route_wait_tx_decision wait_decision;
    struct app_mesh_coordinator_decision coordinator_decision;
    struct app_mesh_route_wait_tx_state wait_state = {
        .channel9_retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,
        .busy_retry_delay_ms = REPORT_TX_RETRY_DELAY_MS,
    };
    uint32_t now_ms;
    uint32_t gateway_ack_wait_retry_delay_ms = 0u;
    bool gateway_ack_send_attempted = false;
    bool gateway_ack_policy_deferred = false;
    bool route_wait_send_attempted = false;
    bool route_wait_policy_deferred = false;
    int ret;

    if (!mesh_route_waiting_tx_valid ||
        (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }
    mesh_coordinator_decide_now("route-wait", &coordinator_decision);
    if (!coordinator_decision.route_wait_allowed) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_WAIT_COORD_DEFER state=%s reason=%s q=%u\n",
                                app_mesh_coordinator_state_name(
                                    coordinator_decision.state),
                                coordinator_decision.reason == NULL ? "none" :
                                coordinator_decision.reason,
                                k_msgq_num_used_get(&mesh_rx_msgq));
        }
        mesh_schedule_route_waiting_retry_after("route-wait-coordinator",
                                                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_tx_handoff_gate_yields(
            APP_MESH_TX_HANDOFF_WORK_ROUTE_WAITING,
            mesh_route_waiting_tx_valid,
            mesh_route_reply_handoff_active(),
            k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            &handoff_result)) {
        status_debug_note("DBG_ROUTE_WAIT_RX_HANDOFF\n");
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_id_is_unicast(mesh_route_ready_event_peer_id)) {
        const struct app_mesh_route_ready_handoff_state route_ready_state = {
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = true,
            .deferred_peer_id = mesh_route_ready_event_peer_id,
        };

        app_mesh_route_ready_handoff_on_waiting_tx(&route_ready_state,
                                                   &route_ready_result);
        status_debug_printf("DBG_ROUTE_READY_WAIT_DECISION rxq=%u def=0x%llx prop=%u waitrx=%u allow=%u evwait=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            (unsigned long long)mesh_route_ready_event_peer_id,
                            route_ready_result.propose_deferred ? 1u : 0u,
                            route_ready_result.schedule_propose_wait_rx ? 1u : 0u,
                            route_ready_result.allow_waiting_tx ? 1u : 0u,
                            route_ready_result.schedule_event_accept_wait ? 1u : 0u);
        if (route_ready_result.schedule_propose_wait_rx) {
            status_debug_note("DBG_ROUTE_READY_PROPOSE_WAIT_RX\n");
            mesh_schedule_route_waiting_retry("route-ready-propose-wait-rx");
            return;
        }

        if (route_ready_result.propose_deferred) {
            int propose_ret;

            status_debug_note("DBG_ROUTE_READY_PROPOSE_DRAINED\n");
            LOG_INF("mesh route-ready deferred proposal after RX drain: next=0x%016llx delay_ms=%u",
                    (unsigned long long)route_ready_result.peer_id,
                    MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            if (route_ready_result.clear_deferred_peer) {
                mesh_route_ready_event_peer_id = 0u;
            }
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            propose_ret = mesh_propose_event_after_channel5_contact(
                route_ready_result.peer_id,
                "route-ready-drained-event-propose");
            app_mesh_route_ready_handoff_after_proposal(propose_ret,
                                                        &route_ready_result);
        }
        if (route_ready_result.schedule_event_accept_wait) {
            mesh_schedule_route_ready_event_retry(
                &route_ready_result,
                "route-ready-event-accept-wait");
            return;
        }
        if (!route_ready_result.allow_waiting_tx) {
            return;
        }
    }

    ret = k_mutex_lock(&mesh_route_wait_scratch_lock, K_NO_WAIT);
    if (ret < 0) {
        mesh_schedule_route_waiting_retry_after("route-wait-scratch-busy",
                                                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }

    *pending = mesh_route_waiting_tx;
    now_ms = k_uptime_get_32();
    wait_state.outbound_ready = mesh_outbound_ready_for_tx(pending, now_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_TRY msg=0x%02x dst=0x%llx ready=%u now=%u early=%u q=%u\n",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.dst_id,
                            wait_state.outbound_ready ? 1u : 0u,
                            now_ms,
                            pending->earliest_tx_ms,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    if (pending->packet.msg_type == MSG_GATEWAY_ACK &&
        mesh_id_is_unicast(pending->next_hop_id)) {
        if (!wait_state.outbound_ready) {
            mesh_schedule_route_waiting_retry("gateway-ack-not-ready");
            goto out_unlock;
        }

        ret = mesh_try_deferred_gateway_ack_on_channel9(
            pending,
            "gateway-ack-deferred",
            &gateway_ack_send_attempted,
            &gateway_ack_policy_deferred,
            &gateway_ack_wait_retry_delay_ms);
        if (ret == 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &pending->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

            app_mesh_rf_retry_note_success(
                &mesh_deferred_gateway_ack_rf_retry,
                &retry_key);
            mesh_route_waiting_tx_valid = false;
            goto out_unlock;
        }
        if (gateway_ack_send_attempted) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &pending->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);
            uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                &mesh_deferred_gateway_ack_rf_retry,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "gateway-ack-send-retry");

            mesh_schedule_route_waiting_retry_after(
                "gateway-ack-send-retry",
                retry_delay_ms);
            goto out_unlock;
        }
        if (ret == -EHOSTUNREACH) {
            (void)mesh_propose_event_after_channel5_contact(
                pending->next_hop_id,
                "gateway-ack-channel9-refresh");
            mesh_schedule_route_waiting_retry_after("gateway-ack-channel9-refresh",
                                                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            goto out_unlock;
        }
        if (ret == -EBUSY) {
            uint32_t retry_delay_ms = gateway_ack_wait_retry_delay_ms;

            if (gateway_ack_policy_deferred) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &pending->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-channel5-deferral");
            }
            if (retry_delay_ms == 0u) {
                retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
            }
            mesh_schedule_route_waiting_retry_after(
                gateway_ack_policy_deferred ?
                    "gateway-ack-channel5-deferral" :
                    "gateway-ack-channel9-wait",
                retry_delay_ms);
            goto out_unlock;
        }

        mesh_schedule_route_waiting_retry_after("gateway-ack-send-retry",
                                                REPORT_TX_RETRY_DELAY_MS);
        goto out_unlock;
    }

    if (wait_state.outbound_ready) {
        wait_state.tx_ret = mesh_start_tracked_tx_with_retry(
            pending,
            "route-discovered-packet",
            NULL,
            app_mesh_route_wait_tx_may_store(
                APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
            &route_wait_send_attempted,
            NULL,
            &route_wait_policy_deferred);
    }
    if (route_wait_send_attempted && wait_state.tx_ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);

        app_mesh_rf_retry_note_success(&mesh_route_wait_delivery_rf_retry,
                                       &retry_key);
    } else if (route_wait_send_attempted) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);
        uint32_t retry_delay_ms = 0u;
        bool policy_override = false;

#if DEVICE_ROLE == ROLE_ANCHOR
        if (wait_state.tx_ret == -EAGAIN) {
            policy_override = report_tx_consume_retry_delay_override(
                &retry_delay_ms);
        }
#endif
        if (!policy_override) {
            retry_delay_ms = mesh_rf_retry_next_delay_ms(
                &mesh_route_wait_delivery_rf_retry,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "route-waiting-send-retry");
        }
        mesh_schedule_route_waiting_retry_after(
            "route-waiting-send-retry",
            retry_delay_ms);
        goto out_unlock;
    }
    if (route_wait_policy_deferred) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &pending->packet,
                APP_MESH_RF_RETRY_OPERATION_ROUTE_WAIT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
            &mesh_route_wait_delivery_rf_retry,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "route-waiting-channel5-deferral");

        mesh_schedule_route_waiting_retry_after(
            "route-waiting-channel5-deferral",
            retry_delay_ms);
        goto out_unlock;
    }
    app_mesh_route_wait_tx_decide(&wait_state, &wait_decision);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_WAIT_DECISION tx=%d action=%u delay=%u req_attempt=%u req_ret=%d\n",
                            wait_state.tx_ret,
                            (unsigned int)wait_decision.action,
                            wait_decision.delay_ms,
                            wait_state.route_request_attempted ? 1u : 0u,
                            wait_state.route_request_ret);
    }
    switch (wait_decision.action) {
    case APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID:
        mesh_route_waiting_tx_valid = false;
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE:
        ret = mesh_request_route(pending->packet.dst_id, wait_decision.reason);
        wait_state.route_request_attempted = true;
        wait_state.route_request_ret = ret;
        app_mesh_route_wait_tx_decide(&wait_state, &wait_decision);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_WAIT_AFTER_REQ req_ret=%d action=%u delay=%u\n",
                                ret,
                                (unsigned int)wait_decision.action,
                                wait_decision.delay_ms);
        }
        if (wait_decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_DROP) {
            mesh_drop_route_waiting_tx(wait_decision.reason);
        }
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_DROP:
        mesh_drop_route_waiting_tx(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY:
        mesh_schedule_route_waiting_retry(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY:
        mesh_schedule_route_waiting_retry_after(wait_decision.reason,
                                                wait_decision.delay_ms);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY:
        mesh_schedule_route_waiting_exhausted_retry(wait_decision.reason);
        break;
    case APP_MESH_ROUTE_WAIT_TX_ACTION_NONE:
    default:
        break;
    }

out_unlock:
    k_mutex_unlock(&mesh_route_wait_scratch_lock);
}

static int mesh_start_tracked_tx_with_retry(const struct mesh_outbound *out,
                                            const char *reason,
                                            uint32_t *wait_retry_delay_ms,
                                            bool store_route_wait,
                                            bool *send_attempted,
                                            bool *rf_sent,
                                            bool *policy_deferred)
{
    struct mesh_outbound aged_out;
    struct mesh_outbound tx;
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint32_t now_ms;
    uint32_t channel9_event_start_ms = 0u;
    uint64_t channel9_next_hop_id = 0u;
    bool channel9_success_pending = false;
    bool channel9_report_latency_pending = false;
    bool direct_gateway_tx_pending = false;
    bool send_prepared_c5_control = false;
    uint8_t send_prepared_c5_purpose = 0u;
    enum mesh_c5_control_send_mode send_prepared_c5_mode =
        MESH_C5_CONTROL_WAKE_IF_NEEDED;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }
    if (rf_sent != NULL) {
        *rf_sent = false;
    }
    if (send_attempted != NULL) {
        *send_attempted = false;
    }
    if (policy_deferred != NULL) {
        *policy_deferred = false;
    }
    if (wait_retry_delay_ms != NULL) {
        *wait_retry_delay_ms = 0u;
    }

    aged_out = *out;
    now_ms = k_uptime_get_32();
    mesh_outbound_refresh_age(&aged_out, now_ms);
    mesh_attach_paused_delivery_loss(&aged_out, reason);

    if (mesh_outbound_needs_result_offer(&aged_out)) {
        ret = mesh_relay_start_result_offer(&mesh_runtime,
                                            &aged_out.packet,
                                            aged_out.payload,
                                            aged_out.payload_len,
                                            now_ms,
                                            &tx);
        if (ret == PROTO_OK) {
            send_prepared_c5_control = true;
            send_prepared_c5_purpose = C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT;
            send_prepared_c5_mode = MESH_C5_CONTROL_WAKE_IF_NEEDED;
            goto send_prepared;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            int route_ret;

            if (store_route_wait) {
                mesh_store_route_waiting_tx(&aged_out);
            }
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_schedule_route_waiting_exhausted_retry(
                    "result-offer-route-exhausted");
                return -EHOSTUNREACH;
            }
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh result offer rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    if (mesh_packet_prefers_channel9(&aged_out.packet)) {
        uint64_t debug_next_hop = 0u;
        int debug_select_ret;

        debug_select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                      aged_out.packet.dst_id,
                                                      &debug_next_hop);
        if (debug_select_ret == PROTO_OK) {
            (void)mesh_advance_channel9_timing_past(debug_next_hop,
                                                    now_ms,
                                                    "tx-select");
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            DEVICE_ROLE != ROLE_GATEWAY &&
            aged_out.packet.dst_id == GATEWAY_ID &&
            debug_select_ret == PROTO_OK &&
            debug_next_hop == GATEWAY_ID) {
            ret = mesh_relay_start_tx(&mesh_runtime,
                                      &aged_out.packet,
                                      aged_out.payload,
                                      aged_out.payload_len,
                                      now_ms,
                                      &tx);
            if (ret == PROTO_OK) {
                tx.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
                if (mesh_relay_tx_active(&mesh_runtime) &&
                    mesh_runtime.pending.packet.session_id == tx.packet.session_id &&
                    mesh_runtime.pending.packet.seq == tx.packet.seq) {
                    mesh_runtime.pending.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
                }
                channel9_success_pending = true;
                direct_gateway_tx_pending = true;
                channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
                channel9_event_start_ms = now_ms;
                channel9_next_hop_id = tx.next_hop_id;
                plan.start_ms = now_ms;
                plan.end_ms = now_ms + MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
                plan.window_ms = MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS;
                status_debug_printf("DBG_DIRECT_GW_TX seq=%u now=%u reason=%s\n",
                                    tx.packet.seq,
                                    now_ms,
                                    reason == NULL ? "mesh-tx" : reason);
                goto send_prepared;
            }
            if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
                int route_ret;

                if (store_route_wait) {
                    mesh_store_route_waiting_tx(&aged_out);
                }
                route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
                if (route_ret == -ETIMEDOUT) {
                    mesh_schedule_route_waiting_exhausted_retry(
                        "direct-gateway-route-exhausted");
                    return -EHOSTUNREACH;
                }
                return -EHOSTUNREACH;
            }
            return mesh_errno_from_proto(ret);
        }
        mesh_fill_channel5_requirements(&requirements);
        ret = mesh_relay_start_channel9_tx(&mesh_runtime,
                                           &aged_out.packet,
                                           aged_out.payload,
                                           aged_out.payload_len,
                                           &requirements,
                                           now_ms,
                                           &plan,
                                           &tx);
        if (ret != PROTO_ERR_NOT_FOUND) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("tx",
                                       reason,
                                       aged_out.packet.dst_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
	        if (ret == PROTO_OK) {
	            uint32_t required_ms = 0u;
	            uint32_t fit_now_ms = k_uptime_get_32();
	            uint32_t event_counter = 0u;

	            tx.earliest_tx_ms = mesh_ch9_slot_send_start_ms(&tx, &plan, fit_now_ms);
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                (void)mesh_debug_channel9_state(tx.next_hop_id, &event_counter, NULL);
	                status_debug_printf("DBG_CH9_TX_SINGLE_SLOT cnt=%u now=%u start=%u end=%u txstart=%u seq=%u\n",
	                                    event_counter,
	                                    fit_now_ms,
	                                    plan.start_ms,
	                                    plan.end_ms,
	                                    tx.earliest_tx_ms,
	                                    tx.packet.seq);
	            }
            if (!mesh_ch9_tx_fits_plan(&tx, &plan, fit_now_ms, &required_ms)) {
                bool deferred = mesh_defer_active_collection_result("channel9-slot-full");

                if (!deferred) {
                    mesh_relay_cancel_tx(&mesh_runtime);
                    app_mesh_persistence_clear_outbox();
                }
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_SINGLE_SLOT_FULL\n");
                    status_debug_note("DBG_CH9_TX_SINGLE_MISSED_SLOT\n");
                    status_debug_printf("DBG_CH9_TX_SINGLE_FULL now=%u end=%u req=%u seq=%u\n",
                                        fit_now_ms,
                                        plan.end_ms,
                                        required_ms,
                                        tx.packet.seq);
                }
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   tx.next_hop_id,
                                   &plan,
                                   reason);
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                tx.next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                return deferred ? 0 : -EBUSY;
            }
            channel9_success_pending = true;
            channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
            channel9_event_start_ms = plan.start_ms;
            channel9_next_hop_id = tx.next_hop_id;
            mesh_ch9_event_set(CH9_EVENT_GRANTED,
                               channel9_next_hop_id,
                               &plan,
                               reason);
            goto send_prepared;
        }
        if (ret == PROTO_ERR_BUSY) {
            if (policy_deferred != NULL) {
                *policy_deferred =
                    mesh_event_plan_is_policy_deferral(plan.action);
            }
            if (wait_retry_delay_ms != NULL &&
                plan.action == MESH_EVENT_PLAN_WAIT) {
                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    wait_retry_delay_ms);
            }
            mesh_debug_channel9_unavailable("tx",
                                            reason,
                                            &aged_out.packet,
                                            ret,
                                            debug_select_ret,
                                            debug_next_hop,
                                            &plan,
                                            now_ms);
            if (mesh_event_plan_debugs_channel5(plan.action)) {
                mesh_ch9_event_set(CH9_EVENT_PREEMPTED_BY_C5,
                                   debug_next_hop,
                                   &plan,
                                   reason);
            }
            return -EBUSY;
        }
        if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
            int route_ret;

            mesh_debug_channel9_unavailable("tx",
                                            reason,
                                            &aged_out.packet,
                                            ret,
                                            debug_select_ret,
                                            debug_next_hop,
                                            &plan,
                                            now_ms);
            LOG_WRN("mesh channel-9 timing unavailable for %s; refreshing channel-5 contact: ret=%d",
                    reason,
                    ret);
            if (store_route_wait) {
                mesh_store_route_waiting_tx(&aged_out);
            }
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_schedule_route_waiting_exhausted_retry(
                    "route-discovery-exhausted");
                return -EHOSTUNREACH;
            }
            return -EHOSTUNREACH;
        }
        LOG_WRN("mesh channel-9 TX rejected for %s: ret=%d", reason, ret);
        return mesh_errno_from_proto(ret);
    }

    ret = mesh_relay_start_tx(&mesh_runtime,
                              &aged_out.packet,
                              aged_out.payload,
                              aged_out.payload_len,
                              now_ms,
                              &tx);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh could not start tracked TX for %s: %d", reason, ret);
        if (ret == PROTO_ERR_NOT_FOUND) {
            int route_ret;

            if (store_route_wait) {
                mesh_store_route_waiting_tx(&aged_out);
            }
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_schedule_route_waiting_exhausted_retry(
                    "route-discovery-exhausted");
                return -EHOSTUNREACH;
            }
        }
        return mesh_errno_from_proto(ret);
    }

send_prepared:
    if (channel9_success_pending) {
        mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                           channel9_next_hop_id,
                           &plan,
                           reason);
    }
    if (send_attempted != NULL) {
        *send_attempted = true;
    }
    if (send_prepared_c5_control) {
        ret = mesh_send_c5_control(&tx,
                                   send_prepared_c5_purpose,
                                   send_prepared_c5_mode,
                                   reason);
    } else if (direct_gateway_tx_pending) {
        ret = mesh_send_direct_gateway_payload_and_wait_ack(
            &tx,
            reason,
            &plan,
            channel9_event_start_ms,
            now_ms,
            channel9_report_latency_pending,
            rf_sent);
        if (ret == 0) {
            return 0;
        }
    } else {
        ret = mesh_send_outbound(&tx, reason);
        if (ret == 0 && rf_sent != NULL) {
            *rf_sent = true;
        }
    }
    if (ret < 0) {
        if (channel9_success_pending && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_FAIL\n");
            status_debug_printf("DBG_CH9_TX_SINGLE_FAIL ret=%d seq=%u\n",
                                ret,
                                tx.packet.seq);
        }
        if (channel9_success_pending) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               channel9_next_hop_id,
                               &plan,
                               reason);
        }
        if (direct_gateway_tx_pending && ret == -EAGAIN) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_SINGLE_RETRYABLE ret=%d seq=%u\n",
                                    ret,
                                    tx.packet.seq);
            }
            app_mesh_test_note_report_tx_retryable(tx.packet.seq, ret);
            return mesh_handle_direct_gateway_retry_policy(&tx, reason, ret);
        }
        if (channel9_success_pending && mesh_send_failure_retryable(ret)) {
            mesh_relay_cancel_tx(&mesh_runtime);
            app_mesh_persistence_clear_outbox();
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_SINGLE_RETRYABLE ret=%d seq=%u\n",
                                    ret,
                                    tx.packet.seq);
            }
            app_mesh_test_note_report_tx_retryable(tx.packet.seq, ret);
            return -EAGAIN;
        }
        if (mesh_defer_active_collection_result("send-failure")) {
            if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
                mesh_relay_note_delivery_failure(&mesh_runtime,
                                                 mesh_runtime.pending.packet.dst_id);
                (void)mesh_request_route(mesh_runtime.pending.packet.dst_id, reason);
            }
            return 0;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        app_mesh_persistence_clear_outbox();
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            int route_ret;

            mesh_relay_note_delivery_failure(&mesh_runtime, tx.packet.dst_id);
            if (store_route_wait) {
                mesh_store_route_waiting_tx(&tx);
            }
            route_ret = mesh_request_route(tx.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_schedule_route_waiting_exhausted_retry(
                    "send-failure-route-exhausted");
                return -EHOSTUNREACH;
            }
        }
        return ret;
    }

    mesh_relay_note_tx_sent(&mesh_runtime, &tx, k_uptime_get_32());
    app_mesh_paused_delivery_note_sent(&mesh_paused_delivery, &tx);
    (void)mesh_save_outbox_durable("tracked-send");
    if (channel9_success_pending) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_TX_SINGLE_SEND_OK\n");
        }
        mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                           channel9_next_hop_id,
                           &plan,
                           reason);
        mesh_note_channel9_local_tx(channel9_next_hop_id, channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms > now_ms ?
                                           channel9_event_start_ms - now_ms : 0u);
        }
        {
            bool batch_ack_wait = mesh_ch9_tx_pending_track_sent(
                &tx,
                k_uptime_get_32() + ROUTE_GATEWAY_ACK_TIMEOUT_MS);
            bool core_ack_wait = app_mesh_ch9_core_ack_wait_active(
                &mesh_runtime.pending,
                mesh_relay_tx_active(&mesh_runtime));

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_ACK_WAIT_SELECT batch=%u core=%u state=%u ch=%u next=0x%llx\n",
                                    batch_ack_wait ? 1u : 0u,
                                    core_ack_wait ? 1u : 0u,
                                    mesh_runtime.pending.state,
                                    mesh_runtime.pending.radio_channel,
                                    (unsigned long long)mesh_runtime.pending.next_hop_id);
            }
            if (batch_ack_wait || core_ack_wait) {
                uint32_t ack_deadline_ms = batch_ack_wait ?
                    mesh_ch9_tx_pending.deadline_ms :
                    mesh_runtime.pending.gateway_ack_deadline_ms;

                mesh_ch9_event_note_persistent_ack_wait(
                    channel9_next_hop_id,
                    ack_deadline_ms,
                    reason);
                mesh_schedule_tx_timeout();
                mesh_schedule_uwb_rx(0u);
                return 0;
            }
        }
    }
    mesh_schedule_tx_timeout();
    return 0;
}

int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
{
    return mesh_start_tracked_tx_with_retry(
        out, reason, NULL,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
        NULL,
        NULL,
        NULL);
}

int mesh_start_owned_tracked_tx(const struct mesh_outbound *out,
                                const char *reason,
                                bool *rf_sent)
{
    return mesh_start_tracked_tx_with_retry(
        out, reason, NULL,
        app_mesh_route_wait_tx_may_store(
            APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL),
        NULL,
        rf_sent,
        NULL);
}

int mesh_owned_tracked_tx_preflight(const struct mesh_outbound *out,
                                    const char *reason,
                                    enum app_mesh_route_wait_tx_owner owner,
                                    uint32_t generation)
{
    uint64_t next_hop_id = 0u;
    int ret;

    if (out == NULL ||
        owner != APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL ||
        generation == 0u || generation != out->packet.session_id) {
        return -EINVAL;
    }
    if (mesh_relay_tx_active(&mesh_runtime)) {
        return -EBUSY;
    }
    ret = mesh_relay_select_next_hop(&mesh_runtime,
                                     out->packet.dst_id,
                                     &next_hop_id);
    if (ret == PROTO_OK) {
        return 0;
    }
    if (ret == PROTO_ERR_NOT_FOUND || ret == PROTO_ERR_STALE) {
        (void)mesh_request_route_owned(out->packet.dst_id, reason, owner,
                                       &out->packet, generation);
        return -EHOSTUNREACH;
    }
    return mesh_errno_from_proto(ret);
}

#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_try_send_report_tx_ch9_batch(void)
{
    struct mesh_outbound *first = &report_tx_worker_scratch;
    struct mesh_outbound *queued = &report_tx_worker_scratch;
    struct mesh_outbound *tx = &report_tx_worker_scratch;
    struct mesh_ch9_slot_tx_context slot_tx = {
        .uwb_window_start_ms = -1,
    };
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    struct app_mesh_coordinator_decision coordinator_decision;
    uint64_t next_hop_id = 0u;
    uint32_t now_ms;
    uint32_t deadline_ms;
    uint32_t send_failure_retry_delay_ms = 0u;
    uint8_t sent_count = 0u;
    uint8_t max_sent;
    bool ack_wait_started = false;
    bool core_ack_wait_started = false;
    bool direct_gateway_batch = false;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        !mesh_ch9_tx_pending_can_start()) {
        return -ENOTSUP;
    }

    ret = k_msgq_peek(&report_tx_msgq, first);
    if (ret != 0 || !mesh_packet_prefers_channel9(&first->packet)) {
        return -ENOTSUP;
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(first, now_ms)) {
        return -EAGAIN;
    }

    ret = mesh_relay_select_next_hop(&mesh_runtime, first->packet.dst_id, &next_hop_id);
    if (ret != PROTO_OK) {
        return -ENOTSUP;
    }
    max_sent = app_mesh_ch9_tx_max_in_flight(&first->packet,
                                             next_hop_id,
                                             MESH_CH9_TX_BATCH_MAX);
    if (app_mesh_ch9_tx_requires_tracked_single(&first->packet,
                                                next_hop_id,
                                                MESH_CH9_TX_BATCH_MAX)) {
        return -ENOTSUP;
    }
    direct_gateway_batch =
        next_hop_id == GATEWAY_ID &&
        first->packet.dst_id == GATEWAY_ID &&
        (first->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
    if (direct_gateway_batch) {
        mesh_init_direct_gateway_batch_plan(&plan, now_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_IMMEDIATE now=%u start=%u end=%u q=%u max=%u reserve=%u\n",
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                report_tx_queue_used(),
                                max_sent,
                                MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS);
        }
    } else {
        (void)mesh_advance_channel9_timing_past(next_hop_id, now_ms, "tx-batch-select");
        mesh_fill_channel5_requirements(&requirements);
        (void)mesh_expire_channel9_timings(now_ms, "queued-ch9-batch");
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   next_hop_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("tx-batch",
                                       "queued-ch9-batch",
                                       next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note(ret == PROTO_ERR_STALE ?
                                  "DBG_CH9_TX_BATCH_STALE\n" :
                                  "DBG_CH9_TX_BATCH_WAIT\n");
                status_debug_printf("DBG_CH9_TX_BATCH_WAIT ret=%d act=%u now=%u start=%u q=%u\n",
                                    ret,
                                    (unsigned int)plan.action,
                                    now_ms,
                                    plan.start_ms,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            if (mesh_event_plan_debugs_channel5(plan.action)) {
                mesh_ch9_event_set(CH9_EVENT_PREEMPTED_BY_C5,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch");
            }
            if (ret == PROTO_ERR_BUSY &&
                mesh_event_plan_is_policy_deferral(plan.action)) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &first->packet,
                        APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
                uint32_t retry_delay_ms;

                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                    &mesh_report_rf_retry_bank,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "report-batch-channel5-deferral");
                app_mesh_test_note_report_tx_backoff(first->packet.seq,
                                                     -EBUSY,
                                                     retry_delay_ms);
                report_tx_schedule_backoff(
                    retry_delay_ms,
                    "report-batch-channel5-deferral");
                return -EALREADY;
            }
            if (ret == PROTO_ERR_BUSY &&
                plan.action == MESH_EVENT_PLAN_WAIT) {
                uint32_t retry_delay_ms =
                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;

                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    &retry_delay_ms);
                report_tx_schedule(retry_delay_ms);
                return -EALREADY;
            }
            return ret == PROTO_ERR_BUSY ? -EBUSY : -ENOTSUP;
        }
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t event_counter = 0u;

        if (!direct_gateway_batch) {
            (void)mesh_debug_channel9_state(next_hop_id, &event_counter, NULL);
        }
        status_debug_printf("DBG_CH9_TX_BATCH_SLOT cnt=%u now=%u start=%u end=%u q=%u max=%u\n",
                            event_counter,
                            now_ms,
                            plan.start_ms,
                            plan.end_ms,
                            k_msgq_num_used_get(&report_tx_msgq),
                            max_sent);
        if (direct_gateway_batch) {
            status_debug_printf("DBG_CH9_TX_BATCH_DIRECT_SELECT cnt=%u reserve=%u rx=%u guard=%u cfg=%u\n",
                                event_counter,
                                MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS,
                                MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS,
                                MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                MESH_CH9_TX_CONFIG_GUARD_MS);
        }
    }
    mesh_ch9_event_set(CH9_EVENT_GRANTED,
                       next_hop_id,
                       &plan,
                       "queued-ch9-batch");

    if (direct_gateway_batch) {
        return mesh_try_send_report_tx_ch9_direct_gateway_batch(&plan,
                                                                next_hop_id,
                                                                max_sent,
                                                                now_ms);
    }

    deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    while (sent_count < max_sent) {
        uint64_t queued_next_hop_id = 0u;
        uint32_t required_ms = 0u;
        uint32_t send_start_ms;
        size_t sent_frame_len = 0u;
        bool fits;

        if (k_msgq_peek(&report_tx_msgq, queued) != 0) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=empty sent=%u now=%u q=%u\n",
                                    sent_count,
                                    k_uptime_get_32(),
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        mesh_coordinator_decide_now("ch9-batch", &coordinator_decision);
        if (!coordinator_decision.mesh_work_allowed) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=coord sent=%u state=%s seq=%u q=%u\n",
                                    sent_count,
                                    app_mesh_coordinator_state_name(
                                        coordinator_decision.state),
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        now_ms = k_uptime_get_32();
        if (!mesh_outbound_ready_for_tx(queued, now_ms) ||
            !mesh_packet_prefers_channel9(&queued->packet)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=blocked sent=%u now=%u ready=%u ch9=%u seq=%u earliest=%u q=%u\n",
                                    sent_count,
                                    now_ms,
                                    mesh_outbound_ready_for_tx(queued, now_ms) ? 1u : 0u,
                                    mesh_packet_prefers_channel9(&queued->packet) ? 1u : 0u,
                                    queued->packet.seq,
                                    queued->earliest_tx_ms,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }
        ret = mesh_relay_select_next_hop(&mesh_runtime,
                                         queued->packet.dst_id,
                                         &queued_next_hop_id);
        if (ret != PROTO_OK || queued_next_hop_id != next_hop_id) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && sent_count > 0u) {
                status_debug_printf("DBG_CH9_TX_BATCH_STOP reason=next-hop sent=%u now=%u ret=%d next=0x%llx want=0x%llx seq=%u q=%u\n",
                                    sent_count,
                                    now_ms,
                                    ret,
                                    (unsigned long long)queued_next_hop_id,
                                    (unsigned long long)next_hop_id,
                                    queued->packet.seq,
                                    k_msgq_num_used_get(&report_tx_msgq));
            }
            break;
        }

        *tx = *queued;
        mesh_outbound_refresh_age(tx, now_ms);
        tx->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        tx->next_hop_id = next_hop_id;
        if (sent_count == 0u) {
            tx->earliest_tx_ms = mesh_ch9_slot_send_start_ms(tx, &plan, now_ms);
        } else {
            tx->earliest_tx_ms = now_ms + MESH_CH9_TX_FRAME_GAP_MS;
        }
        send_start_ms = tx->earliest_tx_ms;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_TARGET now=%u start=%u txstart=%u offset=%u seq=%u\n",
                                now_ms,
                                plan.start_ms,
                                tx->earliest_tx_ms,
                                tx->earliest_tx_ms - plan.start_ms,
                                tx->packet.seq);
        }
        fits = slot_tx.active ?
               mesh_ch9_tx_fits_configured_slot(tx,
                                                &plan,
                                                now_ms,
                                                send_start_ms,
                                                &required_ms) :
               mesh_ch9_tx_fits_plan(tx, &plan, now_ms, &required_ms);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_BATCH_FIT sent=%u now=%u start=%u end=%u txstart=%u req=%u fit=%u seq=%u\n",
                                sent_count,
                                now_ms,
                                plan.start_ms,
                                plan.end_ms,
                                send_start_ms,
                                required_ms,
                                fits ? 1u : 0u,
                                tx->packet.seq);
        }
        if (!fits) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_SLOT_FULL\n");
                if (sent_count == 0u) {
                    status_debug_note("DBG_CH9_TX_BATCH_MISSED_SLOT\n");
                }
                status_debug_printf("DBG_CH9_TX_BATCH_FULL sent=%u now=%u end=%u req=%u seq=%u\n",
                                    sent_count,
                                    now_ms,
                                    plan.end_ms,
                                    required_ms,
                                    tx->packet.seq);
            }
            if (sent_count == 0u) {
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-fit");
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
            break;
        }

        if (!slot_tx.active) {
            ret = mesh_ch9_slot_tx_begin(&slot_tx);
            if (ret < 0) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_BATCH_CONFIG_FAIL\n");
                    status_debug_printf("DBG_CH9_TX_BATCH_CONFIG_FAIL ret=%d sent=%u seq=%u\n",
                                        ret,
                                        sent_count,
                                        tx->packet.seq);
                }
                k_mutex_unlock(&mesh_send_scratch_lock);
                return ret;
            }

            now_ms = k_uptime_get_32();
            if (!mesh_ch9_tx_fits_configured_slot(tx,
                                                  &plan,
                                                  now_ms,
                                                  send_start_ms,
                                                  &required_ms)) {
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_BATCH_SLOT_FULL\n");
                    status_debug_note("DBG_CH9_TX_BATCH_MISSED_SLOT\n");
                    status_debug_printf("DBG_CH9_TX_BATCH_FULL sent=%u now=%u end=%u req=%u seq=%u\n",
                                        sent_count,
                                        now_ms,
                                        plan.end_ms,
                                        required_ms,
                                        tx->packet.seq);
                }
                mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-configured-fit");
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                app_mesh_test_note_ch9_missed();
                report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
        }

        if (sent_count == 0u) {
            mesh_ch9_event_set(CH9_EVENT_TX_PAYLOAD,
                               next_hop_id,
                               &plan,
                               "queued-ch9-batch");
        }
        ret = mesh_send_outbound_preconfigured_ch9_locked(tx,
                                                          "queued-ch9-batch",
                                                          &sent_frame_len);
        if (ret < 0) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &tx->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            send_failure_retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "queued-ch9-batch-send");
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_SEND_FAIL\n");
                status_debug_printf("DBG_CH9_TX_BATCH_FAIL ret=%d sent=%u seq=%u delay=%u\n",
                                    ret,
                                    sent_count,
                                    tx->packet.seq,
                                    send_failure_retry_delay_ms);
            }
            if (sent_count == 0u) {
                mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                                   next_hop_id,
                                   &plan,
                                   "queued-ch9-batch-send");
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                           "queued-ch9-batch-send");
                return -EALREADY;
            }
            break;
        }
        {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &tx->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                                &retry_key);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            uint32_t done_ms = k_uptime_get_32();

            status_debug_printf("DBG_CH9_TX_BATCH_FRAME sent=%u done=%u start=%u end=%u rem=%u len=%u seq=%u\n",
                                sent_count + 1u,
                                done_ms,
                                plan.start_ms,
                                plan.end_ms,
                                uptime_ms_until_deadline(done_ms, plan.end_ms),
                                (unsigned int)sent_frame_len,
                                tx->packet.seq);
        }

        (void)report_tx_queue_discard_head_locked();
        mesh_relay_note_tx_sent(&mesh_runtime, tx, k_uptime_get_32());
        app_mesh_paused_delivery_note_sent(&mesh_paused_delivery, tx);
        if (mesh_ch9_tx_pending_track_sent(tx, deadline_ms)) {
            ack_wait_started = true;
        }
        if (app_mesh_ch9_core_ack_wait_active(
                &mesh_runtime.pending,
                mesh_relay_tx_active(&mesh_runtime))) {
            core_ack_wait_started = true;
        }
        sent_count++;
    }
    mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);

    if (send_failure_retry_delay_ms > 0u) {
        report_tx_schedule_backoff(send_failure_retry_delay_ms,
                                   "queued-ch9-batch-partial-send");
    }

    if (sent_count == 0u) {
        return -EBUSY;
    }

    mesh_note_channel9_local_tx(next_hop_id, plan.start_ms);
    mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                       next_hop_id,
                       &plan,
                       "queued-ch9-batch");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_SENT sent=%u batch_ack=%u core_ack=%u deadline=%u\n",
                            sent_count,
                            ack_wait_started ? 1u : 0u,
                            core_ack_wait_started ? 1u : 0u,
                            deadline_ms);
    }
    if (ack_wait_started || core_ack_wait_started) {
        uint32_t ack_deadline_ms = ack_wait_started ?
            mesh_ch9_tx_pending.deadline_ms :
            mesh_runtime.pending.gateway_ack_deadline_ms;

        mesh_ch9_event_note_persistent_ack_wait(next_hop_id,
                                                ack_deadline_ms,
                                                "queued-ch9-batch");
        mesh_schedule_tx_timeout();
        mesh_schedule_uwb_rx(0u);
    } else {
        report_tx_schedule(0u);
    }
    return 0;
}
#endif

void report_tx_schedule(uint32_t delay_ms)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms = k_uptime_get_32();

    if (report_tx_backoff_until_ms != 0u) {
        if (uptime_deadline_reached(now_ms, report_tx_backoff_until_ms)) {
            report_tx_backoff_until_ms = 0u;
        } else {
            uint32_t remaining_ms =
                uptime_ms_until_deadline(now_ms, report_tx_backoff_until_ms);

            if (delay_ms < remaining_ms) {
                delay_ms = remaining_ms;
            }
        }
    }
    (void)mesh_reschedule_delayable(&report_tx_work, delay_ms);
#else
    ARG_UNUSED(delay_ms);
#endif
}

static void report_tx_schedule_backoff(uint32_t delay_ms, const char *reason)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    uint32_t now_ms;

    now_ms = k_uptime_get_32();
    report_tx_backoff_until_ms = now_ms + delay_ms;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_REPORT_TX_BACKOFF reason=%s delay=%u until=%u\n",
                            reason == NULL ? "report-tx" : reason,
                            delay_ms,
                            report_tx_backoff_until_ms);
    }
    report_tx_schedule(delay_ms);
#else
    ARG_UNUSED(delay_ms);
    ARG_UNUSED(reason);
#endif
}

static void mesh_c5_flood_work_handler(struct k_work *work)
{
    struct mesh_outbound outbound;
    uint8_t purpose;
    const char *reason;
    bool response_priority;
    struct app_mesh_flood_result result = {0};
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (!mesh_c5_flood_deferred.valid) {
        return;
    }

    outbound = mesh_c5_flood_deferred.outbound;
    purpose = mesh_c5_flood_deferred.purpose;
    reason = mesh_c5_flood_deferred.reason;
    response_priority = mesh_c5_flood_deferred.response_priority;

    ret = mesh_send_c5_flood_now(&outbound,
                                 purpose,
                                 reason,
                                 true,
                                 response_priority,
                                 false,
                                 NULL,
                                 &result,
                                 NULL);
    if (result.sent_count == 0u && mesh_send_failure_retryable(ret) &&
        mesh_c5_flood_deferred.retry_count < MESH_C5_DEFERRED_MAX_RETRIES) {
        uint32_t retry_ms = mesh_c5_flood_deferred_retry_ms(&outbound);

        mesh_c5_flood_deferred.retry_count++;
        (void)mesh_reschedule_delayable(&mesh_c5_flood_work,
                                        retry_ms);
        return;
    }

    mesh_c5_flood_deferred.valid = false;
    if (ret == 0 && result.sent_count > 0u) {
        mesh_relay_note_tx_sent(&mesh_runtime, &outbound, k_uptime_get_32());
    } else if (ret < 0) {
        LOG_ERR("deferred C5 flood exhausted: msg=0x%02x ret=%d attempts=%u age_ms=%u",
                outbound.packet.msg_type,
                ret,
                mesh_c5_flood_deferred.retry_count,
                k_uptime_get_32() - mesh_c5_flood_deferred.queued_at_ms);
    }
    app_mesh_rf_retry_reset(&mesh_c5_flood_deferred.rf_retry);
}

uint32_t report_tx_queue_used(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    return (uint32_t)k_msgq_num_used_get(&report_tx_msgq) +
           (report_tx_queue_recovery_valid ? 1u : 0u);
#else
    return 0u;
#endif
}

bool mesh_report_tx_backlog_active(void)
{
    return report_tx_queue_used() > 0u || mesh_ch9_tx_pending.active;
}

bool mesh_report_ch9_ack_wait_active(void)
{
    return mesh_ch9_tx_pending.active;
}

static int mesh_queue_anchor_cir_fragment(struct mesh_outbound *outbound,
                                          uint32_t *queue_depth)
{
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    outbound->queued_at_ms = k_uptime_get_32();
    mesh_attach_paused_delivery_loss(outbound, "queue-anchor-cir-fragment");
    mesh_reclaim_for_local_origin_priority(outbound,
                                           "local-origin-cir-ready",
                                           false);
    ret = k_msgq_put(&report_tx_msgq, outbound, K_NO_WAIT);
    if (ret != 0) {
        return -ENOSPC;
    }
    if (queue_depth != NULL) {
        *queue_depth = (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
    }
    return 0;
#else
    ARG_UNUSED(outbound);
    ARG_UNUSED(queue_depth);
    return -ENOTSUP;
#endif
}

#if DEVICE_ROLE == ROLE_ANCHOR
static void report_tx_work_handler(struct k_work *work)
{
    struct mesh_outbound *outbound = &report_tx_worker_scratch;
    struct app_mesh_coordinator_decision coordinator_decision;
    bool anchor_busy;
    bool survey_busy;
    bool relay_tx_active;
    bool ch9_ack_wait_active;
    bool ch9_ack_send_pending;
    bool route_waiting_active;
    bool report_queue_pending;
    bool rx_queue_pending;
    bool route_handoff_active;
    bool report_policy_deferred = false;
    struct app_mesh_tx_handoff_result handoff_result;
    uint32_t now_ms;
    uint32_t channel9_wait_retry_delay_ms = 0u;
    int ret;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    if (report_tx_queue_recovery_valid &&
        k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT) == 0) {
        if (k_msgq_put(&report_tx_msgq,
                       &report_tx_queue_overflow_dropped,
                       K_NO_WAIT) == 0) {
            report_tx_queue_recovery_valid = false;
        }
        k_mutex_unlock(&report_tx_queue_overflow_lock);
    }
#if DEVICE_ROLE == ROLE_ANCHOR && defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (k_msgq_num_used_get(&report_tx_msgq) < 2u) {
        ret = app_mesh_report_encode_queue_next_cir();
        if (ret < 0 && ret != -ENOENT) {
            LOG_WRN("anchor partial CIR report refill failed: %d", ret);
        }
    }
#endif
    anchor_busy = anchor_uwb_window_active();
    survey_busy = mesh_report_anchor_survey_discovery_is_pending();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    ch9_ack_wait_active = mesh_ch9_tx_pending.active;
    ch9_ack_send_pending =
        app_mesh_ch9_ack_table_any_pending(&mesh_ch9_ack_table);
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    report_queue_pending = k_msgq_num_used_get(&report_tx_msgq) > 0;
    if (report_queue_pending &&
        k_msgq_peek(&report_tx_msgq, outbound) == 0 &&
        mesh_outbound_is_local_origin_priority(outbound)) {
        mesh_reclaim_for_local_origin_priority(outbound,
                                               "local-origin-report-ready",
                                               true);
        relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
        ch9_ack_wait_active = mesh_ch9_tx_pending.active;
        route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                               mesh_route_waiting_tx_active();
    }
    rx_queue_pending = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                        k_msgq_num_used_get(&mesh_rx_msgq) > 0u;
    route_handoff_active = mesh_route_reply_handoff_active();
    mesh_coordinator_decide_now("report-tx", &coordinator_decision);
    if (anchor_busy || !coordinator_decision.report_tx_allowed ||
        route_handoff_active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            report_queue_pending) {
            if (anchor_busy) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ANCHOR\n");
            } else if (rx_queue_pending) {
                status_debug_note("DBG_REPORT_WORK_BUSY_RX\n");
            } else if (route_handoff_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_HANDOFF\n");
            } else if (ch9_ack_send_pending) {
                status_debug_note("DBG_REPORT_WORK_BUSY_QACK\n");
            } else if (route_waiting_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ROUTE\n");
            } else if (ch9_ack_wait_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ACK\n");
            } else if (relay_tx_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_TX\n");
            } else {
                status_debug_note("DBG_REPORT_WORK_BUSY_SURVEY\n");
            }
            if (mesh_tx_handoff_gate_yields(
                    APP_MESH_TX_HANDOFF_WORK_REPORT_QUEUE,
                    report_queue_pending,
                    route_handoff_active,
                    rx_queue_pending,
                    &handoff_result)) {
                return;
            }
            if (ch9_ack_send_pending) {
                mesh_schedule_uwb_rx(0u);
            }
            report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        }
        return;
    }

    ret = mesh_try_send_report_tx_ch9_batch();
    if (ret == 0) {
        return;
    }
    if (ret == -EBUSY) {
        report_tx_schedule(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if (ret == -EALREADY) {
        return;
    }
    if (mesh_send_failure_retryable(ret) &&
        k_msgq_peek(&report_tx_msgq, outbound) == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
            &mesh_report_rf_retry_bank,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "report-batch-send");

        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms, "report-batch-send");
        return;
    }

    ret = k_msgq_peek(&report_tx_msgq, outbound);
    if (ret != 0) {
        report_tx_backoff_until_ms = 0u;
        return;
    }

    {
        uint16_t queue_depth = (uint16_t)k_msgq_num_used_get(&report_tx_msgq);
        uint16_t custody_depth = (uint16_t)(mesh_ch9_tx_pending.count +
            (mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u));

        if ((outbound->packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
            app_stack_workload_diag_click_sample(&outbound->packet,
                                                 queue_depth, custody_depth);
        }
        app_stack_workload_diag_cir_sample(&outbound->packet, queue_depth,
                                           custody_depth);
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(outbound, now_ms)) {
        report_tx_schedule(uptime_ms_until_deadline(now_ms, outbound->earliest_tx_ms));
        return;
    }

    {
        uint64_t selected_next_hop_id = 0u;
        int select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                    outbound->packet.dst_id,
                                                    &selected_next_hop_id);

        if (select_ret != PROTO_OK) {
            enum app_mesh_ch9_timeout_pressure_action pressure_action =
                app_mesh_ch9_timeout_pressure_decide(
                    outbound,
                    true,
                    mesh_runtime_has_transit_reservation(),
                    false,
                    DEVICE_ID);

            if (pressure_action == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
                (void)report_tx_queue_discard_head();
                HIGH_DEBUG_COUNTER_INC(mesh_drop);
                status_debug_printf("DBG_TRANSIT_QUEUE_DROP_NO_UPSTREAM src=0x%llx seq=%u type=0x%02x select=%d q=%u\n",
                                    (unsigned long long)outbound->packet.src_id,
                                    outbound->packet.seq,
                                    outbound->packet.msg_type,
                                    select_ret,
                                    report_tx_queue_used());
                if (report_tx_queue_used() > 0u) {
                    report_tx_schedule(0u);
                }
                return;
            }
            if (pressure_action == APP_MESH_CH9_TIMEOUT_DEFER_LOCAL) {
                status_debug_printf("DBG_LOCAL_QUEUE_DEFER_DOWNSTREAM seq=%u type=0x%02x delay=%u select=%d\n",
                                    outbound->packet.seq,
                                    outbound->packet.msg_type,
                                    ROUTE_PARENT_HOLDDOWN_MS,
                                    select_ret);
                report_tx_schedule_backoff(ROUTE_PARENT_HOLDDOWN_MS,
                                           "downstream-reservation");
                return;
            }
        }
    }

    ret = mesh_start_tracked_tx_with_retry(outbound,
                                           "queued-click-report",
                                           &channel9_wait_retry_delay_ms,
                                           app_mesh_route_wait_tx_may_store(
                                               APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC),
                                           NULL,
                                           NULL,
                                           &report_policy_deferred);
    if (ret == 0) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

        app_mesh_rf_retry_bank_note_success(&mesh_report_rf_retry_bank,
                                            &retry_key);
        report_tx_backoff_until_ms = 0u;
        (void)report_tx_queue_discard_head();
        return;
    }

    if (ret == -EAGAIN) {
        uint32_t retry_delay_ms = 0u;

        if (!report_tx_consume_retry_delay_override(&retry_delay_ms)) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &outbound->packet,
                    APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

            retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
                &mesh_report_rf_retry_bank,
                &retry_key,
                APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                "report-tx-transient");
        }

        LOG_WRN("queued gateway-bound report retrying after transient mesh TX failure");
        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms,
                                   "report-tx-transient");
        return;
    }

    if (ret == -EHOSTUNREACH &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_route_waiting_tx_active()) {
        status_debug_note("DBG_REPORT_QUEUE_TO_ROUTE_WAIT\n");
        (void)report_tx_queue_discard_head();
        return;
    }

    if (ret == -EBUSY && report_policy_deferred) {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);
        uint32_t retry_delay_ms = mesh_rf_retry_bank_next_delay_ms(
            &mesh_report_rf_retry_bank,
            &retry_key,
            APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
            "report-channel5-deferral");

        app_mesh_test_note_report_tx_backoff(outbound->packet.seq,
                                             ret,
                                             retry_delay_ms);
        report_tx_schedule_backoff(retry_delay_ms,
                                   "report-channel5-deferral");
        return;
    }

    if (ret == -EBUSY) {
        uint32_t delay_ms = channel9_wait_retry_delay_ms > 0u ?
                            channel9_wait_retry_delay_ms :
                            REPORT_TX_RETRY_DELAY_MS;

        status_debug_printf("DBG_REPORT_TX_WAIT_CH9_SLOT now=%u delay=%u\n",
                            k_uptime_get_32(),
                            delay_ms);
        report_tx_schedule(delay_ms);
        return;
    }

    if (ret == -EHOSTUNREACH) {
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)report_tx_queue_discard_head();
    {
        const struct app_mesh_rf_retry_key retry_key =
            mesh_rf_retry_packet_key(
                &outbound->packet,
                APP_MESH_RF_RETRY_OPERATION_REPORT_DELIVERY);

        app_mesh_rf_retry_bank_forget(&mesh_report_rf_retry_bank, &retry_key);
    }
    LOG_WRN("queued gateway-bound report dropped after permanent TX error: ret=%d", ret);
    if (mesh_delivery_health.permanent_report_failures < UINT32_MAX) {
        mesh_delivery_health.permanent_report_failures++;
    }
    mesh_delivery_health.last_permanent_error = (uint32_t)(-ret);
    mesh_delivery_health.last_permanent_session_id = outbound->packet.session_id;
    mesh_delivery_health.last_permanent_seq = outbound->packet.seq;
    mesh_delivery_health.last_permanent_msg_type = outbound->packet.msg_type;
    HIGH_DEBUG_COUNTER_INC(mesh_drop);
    if (report_tx_queue_used() > 0u) {
        report_tx_schedule(0u);
    }
}
#endif

int queue_anchor_report(const struct mesh_outbound *outbound)
{
    if (outbound == NULL) {
        return -EINVAL;
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound queued;
    int ret;

    queued = *outbound;
    queued.queued_at_ms = k_uptime_get_32();
    mesh_attach_paused_delivery_loss(&queued, "queue-anchor-report");
    mesh_reclaim_for_local_origin_priority(&queued,
                                           "local-origin-report-queued",
                                           false);

    ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
    if (ret != 0) {
        struct app_mesh_paused_delivery_store_result drop_result;
        int lock_ret;

        lock_ret = k_mutex_lock(&report_tx_queue_overflow_lock, K_NO_WAIT);
        if (lock_ret == 0) {
            bool queued_local_priority =
                mesh_outbound_is_local_origin_priority(&queued);

            if (report_tx_queue_recovery_valid) {
                k_mutex_unlock(&report_tx_queue_overflow_lock);
                return -ENOSPC;
            }
            ret = -ENOSPC;
            for (uint32_t i = 0u;
                 i < REPORT_TX_QUEUE_DEPTH &&
                 k_msgq_get(&report_tx_msgq,
                            &report_tx_queue_overflow_dropped,
                            K_NO_WAIT) == 0;
                 i++) {
                bool dropped_candidate = false;

                if (queued_local_priority) {
                    dropped_candidate =
                        !mesh_outbound_is_local_origin_priority(
                            &report_tx_queue_overflow_dropped);
                } else {
                    dropped_candidate =
                        !mesh_outbound_is_local_origin_priority(
                            &report_tx_queue_overflow_dropped);
                }
                if (dropped_candidate) {
                    ret = 0;
                    break;
                }
                (void)k_msgq_put(&report_tx_msgq,
                                 &report_tx_queue_overflow_dropped,
                                 K_NO_WAIT);
            }
            if (ret != 0 && queued_local_priority) {
                ret = k_msgq_get(&report_tx_msgq,
                                 &report_tx_queue_overflow_dropped,
                                 K_NO_WAIT);
            }
            if (ret == 0) {
                app_mesh_paused_delivery_note_drop(&mesh_paused_delivery,
                                                   &drop_result);
                mesh_attach_paused_delivery_loss(&queued,
                                                 "queue-anchor-report-replace");
                ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
                if (ret == 0) {
                    HIGH_DEBUG_COUNTER_INC(mesh_drop);
                    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                                         "phase=replace-oldest old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u queue_depth=%u",
                                         report_tx_queue_overflow_dropped.packet.msg_type,
                                         report_tx_queue_overflow_dropped.packet.seq,
                                         queued.packet.msg_type,
                                         queued.packet.seq,
                                         drop_result.lost_count,
                                         k_msgq_num_used_get(&report_tx_msgq));
                    LOG_WRN("anchor report queue full; replaced oldest report: old_msg=0x%02x old_seq=%u new_msg=0x%02x new_seq=%u lost=%u",
                            report_tx_queue_overflow_dropped.packet.msg_type,
                            report_tx_queue_overflow_dropped.packet.seq,
                            queued.packet.msg_type,
                            queued.packet.seq,
                            drop_result.lost_count);
                    k_mutex_unlock(&report_tx_queue_overflow_lock);
                    ret = 0;
                    goto queued;
                }
            }
            k_mutex_unlock(&report_tx_queue_overflow_lock);
        }

        app_mesh_paused_delivery_note_drop(&mesh_paused_delivery, &drop_result);
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_REPORT_QUEUE_FULL lost=%u lock=%d ret=%d\n",
                                drop_result.lost_count,
                                lock_ret,
                                ret);
        }
        LOG_WRN("anchor report queue full; gateway-bound report dropped: lost=%u",
                drop_result.lost_count);
        return -ENOSPC;
    }

queued:
    if ((queued.packet.flags & FLAG_COUNT_AS_CLICK) != 0u) {
        uint16_t queue_depth = (uint16_t)k_msgq_num_used_get(&report_tx_msgq);

        app_stack_workload_diag_click_admit(&queued.packet, queue_depth,
                                            queue_depth);
        /* Admission retains the new click beside any earlier report custody. */
        app_stack_workload_diag_click_sample(&queued.packet, queue_depth,
                                             queue_depth);
    }
    high_debug_log_event("ANCHOR_REPORT_QUEUE",
                         "msg=0x%02x dst=0x%016llx seq=%u earliest_tx_ms=%u queue_depth=%u",
                         queued.packet.msg_type,
                         (unsigned long long)queued.packet.dst_id,
                         queued.packet.seq,
                         queued.earliest_tx_ms,
                         k_msgq_num_used_get(&report_tx_msgq));
    LOG_INF("anchor queued gateway-bound report: msg=0x%02x earliest_tx_ms=%u queue_depth=%u",
            queued.packet.msg_type,
            queued.earliest_tx_ms,
            k_msgq_num_used_get(&report_tx_msgq));
    if (!anchor_uwb_window_active()) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t delay_ms = mesh_outbound_ready_for_tx(&queued, now_ms) ?
                            0u :
                            uptime_ms_until_deadline(now_ms, queued.earliest_tx_ms);

        report_tx_schedule(delay_ms);
    }
    return 0;
#else
    return 0;
#endif
}

static bool mesh_send_route_reply_outbound_action(const struct mesh_outbound *route_reply,
                                                  bool backup_valid,
                                                  uint64_t backup_next_hop_id,
                                                  const char *reason)
{
    uint64_t route_reply_acked_next_hop = 0u;
    int ret;

    if (route_reply == NULL) {
        return false;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_ACTION reason=%s next=0x%llx seq=%u backup=%u\n",
                            reason == NULL ? "route-reply" : reason,
                            (unsigned long long)route_reply->next_hop_id,
                            route_reply->packet.seq,
                            backup_valid ? 1u : 0u);
    }
    mesh_route_embedded_wait_before_reply(route_reply);
    if (route_reply->earliest_tx_ms != 0u) {
        uint32_t now_ms = k_uptime_get_32();
        uint32_t delay_ms = uptime_ms_until_deadline(now_ms,
                                                     route_reply->earliest_tx_ms);

        if (delay_ms > 0u) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REPLY_ETA_WAIT delay=%u earliest=%u now=%u\n",
                                    delay_ms,
                                    route_reply->earliest_tx_ms,
                                    now_ms);
            }
            mesh_wait_until_ms(route_reply->earliest_tx_ms);
        }
    }
    if (mesh_id_is_unicast(route_reply->next_hop_id)) {
        mesh_c5_contact_exchange(route_reply->next_hop_id,
                                 C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                 k_uptime_get_32() +
                                 MESH_ROUTE_TEST_ROUTE_REPLY_EXCHANGE_MS,
                                 "route-reply-action");
    }
    ret = mesh_send_route_reply_train(route_reply,
                                      backup_valid,
                                      backup_next_hop_id,
                                      &route_reply_acked_next_hop);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REPLY_ACTION_DONE ret=%d ack=0x%llx reason=%s\n",
                            ret,
                            (unsigned long long)route_reply_acked_next_hop,
                            reason == NULL ? "route-reply" : reason);
    }
    if (ret == 0 && mesh_id_is_unicast(route_reply_acked_next_hop)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            LOG_INF("mesh route reply sent; waiting for route origin to propose channel-9 event: next=0x%016llx",
                    (unsigned long long)route_reply_acked_next_hop);
        } else {
            (void)mesh_propose_event_after_channel5_contact(route_reply_acked_next_hop,
                                                            "route-reply-event-propose");
        }
        return true;
    }

    return false;
}

static bool mesh_send_route_reply_action(const struct mesh_relay_result *result,
                                         const char *reason)
{
    if (result == NULL ||
        (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) == 0u) {
        return false;
    }

    return mesh_send_route_reply_outbound_action(
        &result->route_reply,
        result->route_reply_backup_valid,
        result->route_reply_backup_next_hop_id,
        reason);
}

#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_execute_route_request_action(uint64_t previous_hop_id,
                                              uint32_t reply_deadline_ms)
{
    struct mesh_outbound *route_req = &mesh_route_request_action_tx;
    struct mesh_outbound *route_reply = &mesh_route_request_reply_tx;
    struct mesh_route_capture_identity route_identity = {0};
    uint64_t target_id = GATEWAY_ID;
    uint32_t route_reply_window_ms;
    bool embedded_route_sent = false;
    bool handled = false;
    int ret;

    if (!mesh_payload_find_u64(route_req->payload,
                               route_req->payload_len,
                               TLV_RESPONDER_ID,
                               &target_id)) {
        target_id = GATEWAY_ID;
    }
    if (!mesh_route_capture_identity_from_request(route_req,
                                                  target_id,
                                                  &route_identity)) {
        LOG_ERR("mesh route-request action lacks capture identity: src=0x%016llx target=0x%016llx session=%u",
                (unsigned long long)route_req->packet.src_id,
                (unsigned long long)target_id,
                route_req->packet.session_id);
        goto out;
    }

    if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
        LOG_WRN("mesh route-request action expired before forwarding: src=0x%016llx target=0x%016llx session=%u",
                (unsigned long long)route_req->packet.src_id,
                (unsigned long long)target_id,
                route_req->packet.session_id);
        goto out;
    }
    route_reply_window_ms = uptime_ms_until_deadline(k_uptime_get_32(),
                                                     reply_deadline_ms);

    ret = mesh_try_direct_gateway_route_probe(target_id,
                                              "route-request-rebroadcast-probe",
                                              true,
                                              APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE,
                                              0u);
    if (ret == 0 && mesh_id_is_unicast(previous_hop_id)) {
        memset(route_reply, 0, sizeof(*route_reply));

        ret = mesh_relay_build_route_reply_for_request(&mesh_runtime,
                                                       &route_req->packet,
                                                       route_req->payload,
                                                       route_req->payload_len,
                                                       previous_hop_id,
                                                       k_uptime_get_32(),
                                                       sys_rand32_get(),
                                                       route_reply);
        if (ret == PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_DIRECT_REPLY next=0x%llx seq=%u\n",
                                    (unsigned long long)route_reply->next_hop_id,
                                    route_reply->packet.seq);
            }
            handled = mesh_send_route_reply_outbound_action(route_reply,
                                                            false,
                                                            0u,
                                                            "route-request-direct-probe-reply");
            goto out;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_DIRECT_REPLY_FAIL ret=%d\n",
                                ret);
        }
    }

    ret = mesh_send_route_wake_train(MESH_BROADCAST_ID,
                                     route_req,
                                     &embedded_route_sent,
                                     C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                     "route-request-rebroadcast");
    if (ret < 0) {
        LOG_WRN("mesh route-request rebroadcast wake train failed: target=0x%016llx ret=%d",
                (unsigned long long)target_id,
                ret);
        mesh_restart_role_scan();
        goto out;
    }

    if (embedded_route_sent) {
        bool route_reply_captured = false;
        int listen_ret;

        if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                k_uptime_get_32(),
                reply_deadline_ms);
            listen_ret = mesh_listen_for_route_reply(
                target_id,
                "embedded-route-request-rebroadcast",
                route_reply_window_ms,
                &route_identity,
                &route_reply_captured);
        }

        if (route_reply_captured) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_REQ_REBROADCAST_EMBEDDED_REPLY\n");
            }
            handled = true;
            goto out;
        }
        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh embedded rebroadcast route-reply listen failed: target=0x%016llx ret=%d",
                    (unsigned long long)target_id,
                    listen_ret);
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_TURNAROUND delay=%u\n",
                            MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
        k_msleep(MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS);
    }

    ret = mesh_send_outbound(route_req, "route-request-rebroadcast-control");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_TX ret=%d seq=%u ttl=%u\n",
                            ret,
                            route_req->packet.seq,
                            route_req->packet.ttl);
    }
    if (ret == 0) {
        bool route_reply_captured = false;
        int listen_ret;

        if (uptime_deadline_reached(k_uptime_get_32(), reply_deadline_ms)) {
            listen_ret = -ETIMEDOUT;
        } else {
            route_reply_window_ms = uptime_ms_until_deadline(
                k_uptime_get_32(),
                reply_deadline_ms);
            listen_ret = mesh_listen_for_route_reply(
                target_id,
                "route-request-rebroadcast",
                route_reply_window_ms,
                &route_identity,
                &route_reply_captured);
        }

        if (route_reply_captured && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_REQ_REBROADCAST_REPLY_CAPTURED\n");
        } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_REBROADCAST_REPLY_MISS listen_ret=%d win=%u\n",
                                listen_ret,
                                route_reply_window_ms);
        }
        handled = route_reply_captured || listen_ret == 0;
        goto out;
    }

    LOG_WRN("mesh route-request rebroadcast control TX failed: target=0x%016llx ret=%d",
            (unsigned long long)target_id,
            ret);

out:
    return handled;
}

static bool mesh_route_request_action_same_identity(
    const struct mesh_outbound *left,
    const struct mesh_outbound *right)
{
    return left != NULL && right != NULL &&
           left->packet.msg_type == MSG_ROUTE_REQ &&
           right->packet.msg_type == MSG_ROUTE_REQ &&
           left->packet.src_id == right->packet.src_id &&
           left->packet.dst_id == right->packet.dst_id &&
           left->packet.session_id == right->packet.session_id &&
           left->packet.seq == right->packet.seq;
}

static bool mesh_schedule_route_request_action(
    const struct mesh_relay_result *result,
    const struct mesh_rx_pending *rx,
    bool update_only)
{
    const struct mesh_outbound *next;
    struct app_mesh_route_request_defer_decision defer_decision;
    struct app_mesh_route_request_defer_state defer_state;
    uint32_t delay_ms;
    uint32_t now_ms;
    bool same_identity;
    int ret;

    if (result == NULL || rx == NULL ||
        !mesh_id_is_unicast(rx->previous_hop_id)) {
        return false;
    }
    if (update_only) {
        if ((result->actions & MESH_RELAY_ACTION_UPDATE_ROUTE_REQ) == 0u) {
            return false;
        }
    } else if ((result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) == 0u) {
        return false;
    }
    next = &result->route_request;

    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    same_identity = mesh_route_request_action_pending &&
        mesh_route_request_action_same_identity(&mesh_route_request_action_tx,
                                                next);
    defer_state = (struct app_mesh_route_request_defer_state) {
        .pending_due_ms = mesh_route_request_action_tx.earliest_tx_ms,
        .requested_due_ms = next->earliest_tx_ms,
        .pending_reply_deadline_ms =
            mesh_route_request_action_reply_deadline_ms,
        .requested_reply_deadline_ms =
            rx->received_at_ms + mesh_route_reply_listen_window_ms(next->packet.ttl),
        .pending = mesh_route_request_action_pending,
        .same_identity = same_identity,
        .update_only = update_only,
    };
    app_mesh_route_request_defer_decide(&defer_state, &defer_decision);
    if (defer_decision.action == APP_MESH_ROUTE_REQUEST_DEFER_REJECT) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REQ_DEFER_REJECT update=%u same=%u src=0x%llx seq=%u\n",
                                update_only ? 1u : 0u,
                                same_identity ? 1u : 0u,
                                (unsigned long long)next->packet.src_id,
                                next->packet.seq);
            status_debug_printf("DBG_ROUTE_REQ_DEFER_PENDING src=0x%llx seq=%u session=%u due=%u deadline=%u prev=0x%llx\n",
                                (unsigned long long)mesh_route_request_action_tx.packet.src_id,
                                mesh_route_request_action_tx.packet.seq,
                                mesh_route_request_action_tx.packet.session_id,
                                mesh_route_request_action_tx.earliest_tx_ms,
                                mesh_route_request_action_reply_deadline_ms,
                                (unsigned long long)mesh_route_request_action_previous_hop_id);
            status_debug_printf("DBG_ROUTE_REQ_DEFER_REQUEST due=%u deadline=%u now=%u\n",
                                next->earliest_tx_ms,
                                defer_state.requested_reply_deadline_ms,
                                k_uptime_get_32());
        }
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return false;
    }

    mesh_route_request_action_tx = *next;
    mesh_route_request_action_tx.earliest_tx_ms = defer_decision.due_ms;
    mesh_route_request_action_previous_hop_id = rx->previous_hop_id;
    mesh_route_request_action_reply_deadline_ms =
        defer_decision.reply_deadline_ms;
    mesh_route_request_action_pending = true;
    now_ms = k_uptime_get_32();
    delay_ms = app_mesh_route_request_defer_delay_ms(
        now_ms,
        mesh_route_request_action_tx.earliest_tx_ms);
    ret = mesh_reschedule_delayable(&mesh_route_request_action_work, delay_ms);
    if (ret < 0) {
        mesh_route_request_action_pending = false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DEFER update=%u delay=%u earliest=%u now=%u ret=%d src=0x%llx seq=%u\n",
                            update_only ? 1u : 0u,
                            delay_ms,
                            mesh_route_request_action_tx.earliest_tx_ms,
                            now_ms,
                            ret,
                            (unsigned long long)next->packet.src_id,
                            next->packet.seq);
    }
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
    return ret >= 0;
}

static void mesh_route_request_action_work_handler(struct k_work *work)
{
    uint32_t delay_ms;
    uint32_t now_ms;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    k_mutex_lock(&mesh_route_request_action_scratch_lock, K_FOREVER);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ROUTE_REQ_DEFER_WORK pending=%u seq=%u due=%u deadline=%u now=%u\n",
                            mesh_route_request_action_pending ? 1u : 0u,
                            mesh_route_request_action_tx.packet.seq,
                            mesh_route_request_action_tx.earliest_tx_ms,
                            mesh_route_request_action_reply_deadline_ms,
                            k_uptime_get_32());
    }
    if (!mesh_route_request_action_pending) {
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return;
    }
    now_ms = k_uptime_get_32();
    delay_ms = app_mesh_route_request_defer_delay_ms(
        now_ms,
        mesh_route_request_action_tx.earliest_tx_ms);
    if (delay_ms > 0u) {
        (void)mesh_reschedule_delayable(&mesh_route_request_action_work,
                                        delay_ms);
        k_mutex_unlock(&mesh_route_request_action_scratch_lock);
        return;
    }

    mesh_route_request_action_pending = false;
    (void)mesh_execute_route_request_action(
        mesh_route_request_action_previous_hop_id,
        mesh_route_request_action_reply_deadline_ms);
    k_mutex_unlock(&mesh_route_request_action_scratch_lock);
}
#else
static bool mesh_schedule_route_request_action(
    const struct mesh_relay_result *result,
    const struct mesh_rx_pending *rx,
    bool update_only)
{
    ARG_UNUSED(result);
    ARG_UNUSED(rx);
    ARG_UNUSED(update_only);
    return false;
}
#endif

static bool mesh_forward_uses_gateway_batch_queue(const struct mesh_outbound *out)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           DEVICE_ROLE == ROLE_ANCHOR &&
           out != NULL &&
           out->packet.dst_id == GATEWAY_ID &&
           out->next_hop_id == GATEWAY_ID &&
           mesh_packet_prefers_channel9(&out->packet) &&
           (out->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u;
}

static bool mesh_forward_uses_child_gateway_ack_queue(
    const struct mesh_outbound *out,
    uint8_t received_radio_channel)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           DEVICE_ROLE == ROLE_ANCHOR &&
           received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
           out != NULL &&
           out->packet.msg_type == MSG_GATEWAY_ACK &&
           out->packet.src_id == GATEWAY_ID &&
           out->packet.dst_id != DEVICE_ID &&
           out->packet.dst_id != GATEWAY_ID &&
           mesh_id_is_unicast(out->next_hop_id);
}

static bool mesh_parent_contact_failure_is_hard(int ret)
{
    return ret == -ETIMEDOUT || ret == -EHOSTUNREACH ||
           ret == -ENOTCONN || ret == -ECONNRESET;
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result,
                                       uint8_t received_radio_channel,
                                       const struct mesh_rx_pending *rx,
                                       const struct proto_packet *confirmed_packet)
{
    bool forward_sent = false;
    bool forward_queued_gateway_ack = false;
    bool child_custody_ready = true;
    struct app_mesh_result_handoff_ops handoff_ops = {
        .save_child_custody = mesh_handoff_save_child_custody,
        .note_result_bundle_forwarded = mesh_handoff_note_result_bundle_forwarded,
        .send_result_grant = mesh_handoff_send_result_grant,
        .note_tx_sent = mesh_handoff_note_tx_sent,
    };
    struct app_mesh_result_handoff_status handoff_status;

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound *gateway_ack = &mesh_result_action_tx;
        struct mesh_channel5_requirements requirements;
        struct mesh_event_plan plan = {0};
        struct app_mesh_gateway_ack_state ack_state = {
            .route_test_enabled = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST),
            .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
            .received_on_channel9 = received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD,
            .channel9_require_ret = PROTO_ERR_BUSY,
            .channel9_retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,
        };
        struct app_mesh_gateway_ack_decision ack_decision;
        bool gateway_ack_policy_deferred = false;
        uint32_t now_ms;
        int ret;

        *gateway_ack = result->gateway_ack;
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            rx != NULL) {
            struct mesh_ch9_batch_metadata batch_metadata;
            int meta_ret = mesh_ch9_batch_metadata_parse(rx->payload,
                                                         rx->payload_len,
                                                         &batch_metadata);

            status_debug_printf("DBG_GATEWAY_ACK_INPUT type=%u src=0x%llx dst=0x%llx rxseq=%u ackseq=%u len=%u flags=0x%02x meta=%d\n",
                                rx->packet.msg_type,
                                (unsigned long long)rx->packet.src_id,
                                (unsigned long long)rx->packet.dst_id,
                                rx->packet.seq,
                                gateway_ack->packet.seq,
                                rx->payload_len,
                                rx->packet.flags,
                                meta_ret);

            if (meta_ret == PROTO_OK && batch_metadata.present) {
                gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
                mesh_ch9_ack_batch_queue(gateway_ack, rx);
                status_debug_printf("DBG_GATEWAY_BATCH_ACK_QUEUE batch=%u final=%u n=%u rxseq=%u ackseq=%u next=0x%llx\n",
                                    batch_metadata.batch_id,
                                    batch_metadata.final_packet ? 1u : 0u,
                                    mesh_ch9_ack_batch_count_for_peer(
                                        gateway_ack->next_hop_id),
                                    rx->packet.seq,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->next_hop_id);
                if (batch_metadata.final_packet) {
                    ret = mesh_send_current_ch9_ack_batch(
                        gateway_ack->next_hop_id,
                        "gateway-batch-ack-final");
                    status_debug_printf("DBG_GATEWAY_BATCH_ACK_FINAL batch=%u ret=%d n=%u\n",
                                        batch_metadata.batch_id,
                                        ret,
                                        mesh_ch9_ack_batch_count_for_peer(
                                            gateway_ack->next_hop_id));
                }
                goto after_gateway_ack;
            }
            if (meta_ret != PROTO_OK) {
                status_debug_printf("DBG_GATEWAY_BATCH_ACK_META_BAD ret=%d rxseq=%u len=%u\n",
                                    meta_ret,
                                    rx->packet.seq,
                                    rx->payload_len);
            }
        }
        if (DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            const uint64_t ack_deadline_ms = (uint64_t)k_uptime_get() +
                MESH_RELAY_GATEWAY_ACK_RETRY_BUDGET_MAX_MS;

            gateway_ack->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
            gateway_ack->earliest_tx_ms = 0u;
            ret = app_node_comm_submit_control_response(
                gateway_ack,
                ack_deadline_ms,
                gateway_ack->packet.seq);
            status_debug_printf("DBG_GATEWAY_ACK_QUEUE ret=%d seq=%u dst=0x%llx next=0x%llx deadline=%llu\n",
                                ret,
                                gateway_ack->packet.seq,
                                (unsigned long long)gateway_ack->packet.dst_id,
                                (unsigned long long)gateway_ack->next_hop_id,
                                (unsigned long long)ack_deadline_ms);
            if (ret < 0) {
                LOG_WRN("gateway ACK communication admission failed; sender retains custody: ret=%d dst=0x%016llx seq=%u",
                        ret,
                        (unsigned long long)gateway_ack->packet.dst_id,
                        gateway_ack->packet.seq);
            }
            goto after_gateway_ack;
        }
        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_QUEUE_ROUTE_TEST_ACK) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            mesh_ch9_ack_batch_queue(gateway_ack, rx);
            goto after_gateway_ack;
        }

        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE == ROLE_GATEWAY &&
                received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
                status_debug_printf("DBG_GATEWAY_ACK_GUARD ms=%u seq=%u\n",
                                    MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS,
                                    gateway_ack->packet.seq);
                k_msleep(MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS);
            }
            ret = mesh_send_outbound_keep_channel9_awake(
                gateway_ack, ack_decision.reason, NULL);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                DEVICE_ROLE == ROLE_GATEWAY) {
                status_debug_printf("DBG_GATEWAY_ACK_SEND ret=%d seq=%u dst=0x%llx next=0x%llx\n",
                                    ret,
                                    gateway_ack->packet.seq,
                                    (unsigned long long)gateway_ack->packet.dst_id,
                                    (unsigned long long)gateway_ack->next_hop_id);
            }
            if (ret == 0) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                app_mesh_rf_retry_note_success(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                mesh_note_channel9_local_tx(gateway_ack->next_hop_id, k_uptime_get_32());
                high_debug_log_event("GATEWAY_ACK_TX",
                                     "mode=current-channel9 dst=0x%016llx next=0x%016llx seq=%u",
                                     (unsigned long long)gateway_ack->packet.dst_id,
                                     (unsigned long long)gateway_ack->next_hop_id,
                                     gateway_ack->packet.seq);
                LOG_INF("gateway ACK sent on current channel-9 event: dst=0x%016llx next=0x%016llx seq=%u",
                        (unsigned long long)gateway_ack->packet.dst_id,
                        (unsigned long long)gateway_ack->next_hop_id,
                        gateway_ack->packet.seq);
                goto after_gateway_ack;
            } else {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-current-channel9");

                LOG_WRN("gateway ACK current channel-9 send failed; will retry on channel-9 event: next=0x%016llx",
                        (unsigned long long)gateway_ack->next_hop_id);
                ack_state.current_channel9_attempted = true;
                ack_state.current_channel9_ret = ret;
                app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
                mesh_store_route_waiting_tx(gateway_ack);
                mesh_schedule_route_waiting_retry_after(ack_decision.reason,
                                                        retry_delay_ms);
                goto after_gateway_ack;
            }
        }

        mesh_fill_channel5_requirements(&requirements);
        now_ms = k_uptime_get_32();
        (void)mesh_expire_channel9_timings(now_ms, "gateway-ack");
        ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                   gateway_ack->next_hop_id,
                                                   &requirements,
                                                   now_ms,
                                                   &plan);
        ack_state.channel9_require_ret = ret;
        app_mesh_gateway_ack_decide(&ack_state, &ack_decision);
        if (ret != PROTO_ERR_STALE) {
            mesh_event_note_plan_action(&mesh_event_stats, plan.action);
        }
        mesh_debug_channel5_preemption("gateway-ack",
                                       "gateway-ack",
                                       gateway_ack->next_hop_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        gateway_ack_policy_deferred =
            ret == PROTO_ERR_BUSY &&
            mesh_event_plan_is_policy_deferral(plan.action);
        if (gateway_ack_policy_deferred) {
            mesh_relay_note_channel9_missed(&mesh_runtime,
                                            gateway_ack->next_hop_id,
                                            &mesh_event_stats);
        }
        if (ack_decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &gateway_ack->packet,
                    APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_send_outbound(gateway_ack, ack_decision.reason);
            if (ret == 0) {
                app_mesh_rf_retry_note_success(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key);
                mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                mesh_note_channel9_local_tx(gateway_ack->next_hop_id, plan.start_ms);
            } else {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-planned-channel9");

                mesh_store_route_waiting_tx(gateway_ack);
                mesh_schedule_route_waiting_retry_after(
                    "gateway-ack-planned-channel9",
                    retry_delay_ms);
            }
        } else if (ack_decision.action ==
                   APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9) {
            LOG_WRN("gateway ACK deferred until channel-9 timing refresh: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack->next_hop_id,
                    ret);
            mesh_store_route_waiting_tx(gateway_ack);
            (void)mesh_propose_event_after_channel5_contact(gateway_ack->next_hop_id,
                                                            ack_decision.reason);
            mesh_schedule_route_waiting_retry_after(ack_decision.reason,
                                                    MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        } else if (ack_decision.action ==
                   APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY) {
            uint32_t retry_delay_ms = ack_decision.delay_ms;
            const char *retry_reason = ack_decision.reason;

            if (gateway_ack_policy_deferred) {
                const struct app_mesh_rf_retry_key retry_key =
                    mesh_rf_retry_packet_key(
                        &gateway_ack->packet,
                        APP_MESH_RF_RETRY_OPERATION_DEFERRED_GATEWAY_ACK);

                retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_deferred_gateway_ack_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "gateway-ack-channel5-deferral");
                retry_reason = "gateway-ack-channel5-deferral";
            } else if (ret == PROTO_ERR_BUSY &&
                       plan.action == MESH_EVENT_PLAN_WAIT) {
                (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                    now_ms,
                    plan.start_ms,
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                    &retry_delay_ms);
            }
            if (retry_delay_ms == 0u) {
                retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
            }
            LOG_WRN("gateway ACK waiting for channel-9 event: next=0x%016llx ret=%d",
                    (unsigned long long)gateway_ack->next_hop_id,
                    ret);
            mesh_store_route_waiting_tx(gateway_ack);
            mesh_schedule_route_waiting_retry_after(retry_reason,
                                                    retry_delay_ms);
        }
    }
after_gateway_ack:
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        int ret;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            result->forward.packet.msg_type == MSG_GATEWAY_ACK) {
            status_debug_printf("DBG_FORWARD_GATEWAY_ACK_CAND ch=%u src=0x%llx dst=0x%llx next=0x%llx local=0x%llx gw=0x%llx\n",
                                received_radio_channel,
                                (unsigned long long)result->forward.packet.src_id,
                                (unsigned long long)result->forward.packet.dst_id,
                                (unsigned long long)result->forward.next_hop_id,
                                (unsigned long long)DEVICE_ID,
                                (unsigned long long)GATEWAY_ID);
        }
        if (result->forward.packet.dst_id == MESH_BROADCAST_ID) {
            ret = mesh_send_c5_flood_response(
                &result->forward,
                mesh_c5_purpose_for_packet(&result->forward.packet),
                "broadcast-forward");
        } else if (mesh_forward_uses_child_gateway_ack_queue(
                       &result->forward,
                       received_radio_channel)) {
            struct mesh_outbound *forward_ack = &mesh_result_action_tx;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_FORWARD_GATEWAY_ACK_MATCH\n");
            }
            *forward_ack = result->forward;
            forward_ack->radio_channel = MESH_EVENT_CHANNEL;
            ret = mesh_ch9_ack_batch_queue_forwarded_gateway_ack(
                forward_ack,
                "forward-gateway-ack");
            forward_queued_gateway_ack = ret == 0;
        } else if (mesh_forward_uses_gateway_batch_queue(&result->forward)) {
            ret = queue_anchor_report(&result->forward);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_FORWARD_GATEWAY_BATCH_QUEUE ret=%d seq=%u q=%u\n",
                                    ret,
                                    result->forward.packet.seq,
                                    report_tx_queue_used());
            }
        } else {
            ret = mesh_start_tracked_tx(&result->forward, "forward");
        }
        forward_sent = ret == 0 && !forward_queued_gateway_ack;
        app_mesh_result_handoff_after_forward(result,
                                              forward_sent,
                                              DEVICE_ROLE == ROLE_ANCHOR,
                                              &handoff_ops,
                                              &handoff_status);
        if (handoff_status.child_custody_save_failed) {
            LOG_WRN("mesh child custody snapshot update failed after forward");
        }
    }
    if (child_custody_ready &&
        (result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) != 0u) {
        app_mesh_result_handoff_prepare_hop_ack(result,
                                                forward_sent,
                                                DEVICE_ROLE == ROLE_ANCHOR,
                                                &handoff_ops,
                                                &handoff_status);
        child_custody_ready = handoff_status.child_custody_ready;
        if (handoff_status.child_custody_save_failed) {
            LOG_WRN("mesh hop ACK skipped: child custody snapshot unavailable");
        }
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) &&
        handoff_status.hop_ack_allowed) {
        struct mesh_outbound *hop_ack = &mesh_result_action_tx;

        *hop_ack = result->hop_ack;
        if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            mesh_ch9_ack_batch_queue(hop_ack, rx);
        } else if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            hop_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(hop_ack, "hop-ack") == 0) {
                mesh_relay_note_tx_sent(&mesh_runtime, hop_ack, k_uptime_get_32());
            }
        } else if (mesh_send_outbound(hop_ack, "hop-ack") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, hop_ack, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY_ACK) {
        struct mesh_outbound *route_reply_ack = &mesh_result_action_tx;
        bool active_exchange;
        int ret;

        *route_reply_ack = result->route_reply_ack;
        active_exchange = mesh_c5_contact_peer_active(route_reply_ack->next_hop_id,
                                                      k_uptime_get_32());
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_TX_BEGIN next=0x%llx seq=%u active=%u\n",
                                (unsigned long long)route_reply_ack->next_hop_id,
                                route_reply_ack->packet.seq,
                                active_exchange ? 1u : 0u);
        }
        ret = mesh_send_c5_control(route_reply_ack,
                                   C5_CONTACT_PURPOSE_ROUTE_REPLY,
                                   MESH_C5_CONTROL_ACCEPTED_EXCHANGE,
                                   "route-reply-ack");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_REPLY_ACK_TX_DONE ret=%d next=0x%llx seq=%u\n",
                                ret,
                                (unsigned long long)route_reply_ack->next_hop_id,
                                route_reply_ack->packet.seq);
        }
        if (ret == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime,
                                    route_reply_ack,
                                    k_uptime_get_32());
        }
        mesh_c5_contact_clear(ret == 0 ? "route-reply-ack-sent" :
                                       "route-reply-ack-failed");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        (void)mesh_send_route_reply_action(result, "route-reply-action");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, false);
    }
    if (result->actions & MESH_RELAY_ACTION_UPDATE_ROUTE_REQ) {
        (void)mesh_schedule_route_request_action(result, rx, true);
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ROUTE_ADV) {
        int ret;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_BEGIN ttl=%u seq=%u earliest=%u\n",
                                result->gateway_route_adv.packet.ttl,
                                result->gateway_route_adv.packet.seq,
                                result->gateway_route_adv.earliest_tx_ms);
        }
        ret = mesh_send_c5_flood_response(&result->gateway_route_adv,
                                          C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                                          "gateway-route-adv-forward");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_FORWARD_DONE ret=%d seq=%u\n",
                                ret,
                                result->gateway_route_adv.packet.seq);
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RELAY_BUSY) {
        if (mesh_send_c5_control(&result->busy,
                                 C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH,
                                 MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                 "relay-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_RESULT_BUSY) {
        if (mesh_send_c5_control(&result->busy,
                                 C5_CONTACT_PURPOSE_RESULT_OFFER_GRANT,
                                 MESH_C5_CONTROL_WAKE_IF_NEEDED,
                                 "result-busy") == 0) {
            mesh_relay_note_tx_sent(&mesh_runtime, &result->busy, k_uptime_get_32());
        }
    }
    if (child_custody_ready &&
        (result->actions & MESH_RELAY_ACTION_SEND_RESULT_GRANT)) {
        app_mesh_result_handoff_result_grant(result,
                                             DEVICE_ROLE == ROLE_ANCHOR,
                                             &handoff_ops,
                                             &handoff_status);
        if (handoff_status.result_grant_suppressed) {
            LOG_WRN("mesh result grant skipped: child custody snapshot unavailable");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        struct mesh_outbound *retransmit = &mesh_result_action_tx;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
        uint32_t retry_delay_ms = MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS;
        int ret = PROTO_OK;

        *retransmit = result->retransmit;
        if (mesh_packet_prefers_channel9(&retransmit->packet)) {
            struct mesh_channel5_requirements requirements;
            uint32_t now_ms = k_uptime_get_32();
            uint64_t debug_next_hop = 0u;
            int debug_select_ret;

            debug_select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                          retransmit->packet.dst_id,
                                                          &debug_next_hop);
            mesh_fill_channel5_requirements(&requirements);
            (void)mesh_expire_channel9_timings(now_ms, "retransmit");
            ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                       retransmit->next_hop_id,
                                                       &requirements,
                                                       now_ms,
                                                       &plan);
            if (ret != PROTO_ERR_STALE) {
                mesh_event_note_plan_action(&mesh_event_stats, plan.action);
            }
            mesh_debug_channel5_preemption("retransmit",
                                           "retransmit",
                                           retransmit->next_hop_id,
                                           &requirements,
                                           &plan,
                                           now_ms);
            if (ret == PROTO_OK) {
                retransmit->radio_channel = MESH_EVENT_CHANNEL;
                channel9_replanned = true;
            } else if (ret == PROTO_ERR_BUSY) {
                bool policy_deferred =
                    mesh_event_plan_is_policy_deferral(plan.action);
                int defer_ret;

                if (policy_deferred) {
                    const struct app_mesh_rf_retry_key retry_key =
                        mesh_rf_retry_packet_key(
                            &retransmit->packet,
                            APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);

                    mesh_relay_note_channel9_missed(&mesh_runtime,
                                                    retransmit->next_hop_id,
                                                    &mesh_event_stats);
                    retry_delay_ms = mesh_rf_retry_next_delay_ms(
                        &mesh_retransmit_rf_retry,
                        &retry_key,
                        APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                        "retransmit-channel5-deferral");
                } else if (plan.action == MESH_EVENT_PLAN_WAIT) {
                    (void)app_mesh_ch9_wait_plan_retry_delay_ms(
                        now_ms,
                        plan.start_ms,
                        MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                        &retry_delay_ms);
                }
                LOG_INF("mesh retransmit waiting for channel-9 event: msg=0x%02x dst=0x%016llx start_ms=%u window_ms=%u",
                        retransmit->packet.msg_type,
                        (unsigned long long)retransmit->packet.dst_id,
                        plan.start_ms,
                        plan.window_ms);
                defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    now_ms + retry_delay_ms);
                status_debug_printf("DBG_RETRANSMIT_DEFER reason=%s ret=%d delay=%u seq=%u state=%u\n",
                                    policy_deferred ?
                                        "channel5-deferral" : "ch9-wait",
                                    defer_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
            } else {
                int defer_ret;

                mesh_debug_channel9_unavailable("retransmit",
                                                "retransmit",
                                                &retransmit->packet,
                                                ret,
                                                debug_select_ret,
                                                debug_next_hop,
                                                &plan,
                                                now_ms);
                LOG_WRN("mesh retransmit deferred until channel-9 timing is refreshed: msg=0x%02x dst=0x%016llx ret=%d",
                        retransmit->packet.msg_type,
                        (unsigned long long)retransmit->packet.dst_id,
                        ret);
                retry_delay_ms = RELAY_BUSY_RETRY_MIN_MS;
                defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    now_ms + retry_delay_ms);
                status_debug_printf("DBG_RETRANSMIT_DEFER reason=ch9-refresh ret=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
                if (mesh_channel9_connection_count() == 0u) {
                    if (debug_select_ret == PROTO_OK &&
                        mesh_id_is_unicast(debug_next_hop) &&
                        debug_next_hop != GATEWAY_ID) {
                        uint32_t failure_actions = MESH_RELAY_ACTION_NONE;
                        int failure_status = PROTO_OK;
                        int repair_ret;
                        int failure_ret = PROTO_OK;

                        mesh_route_ready_event_peer_id = debug_next_hop;
                        repair_ret = mesh_propose_event_after_channel5_contact(
                            debug_next_hop,
                            "retransmit-event-repair");
                        if (repair_ret == 0) {
                            mesh_route_ready_event_peer_id = 0u;
                            defer_ret = mesh_relay_note_retransmit_deferred(
                                &mesh_runtime,
                                retransmit,
                                k_uptime_get_32() + RELAY_BUSY_RETRY_MIN_MS);
                        } else if (mesh_parent_contact_failure_is_hard(
                                       repair_ret)) {
                            uint64_t downstream_peers[MESH_RELAY_EVENT_TIMINGS];
                            uint32_t failure_now_ms = k_uptime_get_32();
                            uint8_t downstream_peer_count =
                                mesh_capture_downstream_channel9_peers(
                                    downstream_peers,
                                    ARRAY_SIZE(downstream_peers));

                            failure_ret = mesh_relay_note_pending_parent_failure_status(
                                &mesh_runtime,
                                failure_now_ms,
                                sys_rand32_get(),
                                &failure_actions,
                                &failure_status);
                            defer_ret = failure_ret;
                            if (failure_ret == PROTO_OK) {
                                mesh_notify_removed_downstream_peers(
                                    downstream_peers,
                                    downstream_peer_count,
                                    "parent-route-failed");
                            }
                            if (failure_ret == PROTO_OK &&
                                (failure_actions &
                                 MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
                                mesh_schedule_async_route_request(
                                    retransmit->packet.dst_id,
                                    "retransmit-event-repair-failed");
                            } else if (failure_ret != PROTO_OK) {
                                status_debug_note(
                                    "DBG_RETRANSMIT_EVENT_REPAIR_ACCOUNT_FAIL\n");
                                mesh_relay_invalidate_upstream_route(
                                    &mesh_runtime);
                                mesh_notify_removed_downstream_peers(
                                    downstream_peers,
                                    downstream_peer_count,
                                    "parent-route-account-failed");
                                mesh_schedule_async_route_request(
                                    retransmit->packet.dst_id,
                                    "retransmit-event-repair-account-failed");
                            }
                        } else {
                            defer_ret = mesh_relay_note_retransmit_deferred(
                                &mesh_runtime,
                                retransmit,
                                k_uptime_get_32() + RELAY_BUSY_RETRY_MIN_MS);
                        }
                        status_debug_printf("DBG_RETRANSMIT_EVENT_REPAIR peer=0x%llx repair=%d account=%d actions=0x%08x defer=%d retry=%u\n",
                                            (unsigned long long)debug_next_hop,
                                            repair_ret,
                                            failure_ret,
                                            failure_ret == PROTO_OK &&
                                                mesh_parent_contact_failure_is_hard(
                                                    repair_ret) ?
                                                failure_actions : 0u,
                                            defer_ret,
                                            mesh_runtime.pending.retry_after_ms);
                    } else {
                        (void)mesh_request_route(
                            retransmit->packet.dst_id,
                            "retransmit-channel9-refresh");
                    }
                }
            }
        }
        if (ret == PROTO_OK) {
            const struct app_mesh_rf_retry_key retry_key =
                mesh_rf_retry_packet_key(
                    &retransmit->packet,
                    APP_MESH_RF_RETRY_OPERATION_RETRANSMIT);
            bool backend_rf_started = false;
            bool backend_terminal = false;
            int backend_ret = app_node_comm_backend_retry_preflight(
                &retransmit->packet);
            int account_ret;
            int send_ret;

            if (backend_ret == -ETIMEDOUT || backend_ret == -ECANCELED) {
                backend_terminal = true;
                status_debug_printf(
                    "DBG_NODE_COMM_RETRANSMIT_TERMINAL ret=%d seq=%u\n",
                    backend_ret,
                    retransmit->packet.seq);
                send_ret = backend_ret;
            } else if (backend_ret < 0) {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_retransmit_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "retransmit-facade-preflight");
                int defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    k_uptime_get_32() + retry_delay_ms);

                status_debug_printf("DBG_RETRANSMIT_DEFER reason=facade-preflight ret=%d check=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    backend_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
                send_ret = backend_ret;
            } else {
                send_ret = mesh_send_outbound_with_release(
                    retransmit,
                    "retransmit",
                    MESH_RADIO_RELEASE_STANDBY,
                    &backend_rf_started);
                account_ret = app_node_comm_complete_backend_attempt(
                    &retransmit->packet, backend_rf_started);

                if (account_ret < 0 && account_ret != -ENOENT) {
                    status_debug_printf(
                        "DBG_NODE_COMM_BACKEND_RF_ACCOUNT ret=%d rf=%u seq=%u\n",
                        account_ret,
                        backend_rf_started ? 1u : 0u,
                        retransmit->packet.seq);
                }
                if (account_ret == -ETIMEDOUT ||
                    account_ret == -ECANCELED) {
                    backend_terminal = true;
                    send_ret = account_ret;
                }
            }

            if (send_ret == 0) {
                app_mesh_rf_retry_note_success(&mesh_retransmit_rf_retry,
                                               &retry_key);
                HIGH_DEBUG_COUNTER_INC(mesh_retry);
                mesh_relay_note_tx_sent(&mesh_runtime,
                                        retransmit,
                                        k_uptime_get_32());
                if (channel9_replanned) {
                    mesh_note_channel9_local_tx(retransmit->next_hop_id,
                                                plan.start_ms);
                }
            } else if (!backend_terminal &&
                       backend_ret >= 0) {
                uint32_t retry_delay_ms = mesh_rf_retry_next_delay_ms(
                    &mesh_retransmit_rf_retry,
                    &retry_key,
                    APP_MESH_RF_RETRY_POLICY_RELIABLE_DATA,
                    "retransmit-send-failed");
                int defer_ret = mesh_relay_note_retransmit_deferred(
                    &mesh_runtime,
                    retransmit,
                    k_uptime_get_32() + retry_delay_ms);

                status_debug_printf("DBG_RETRANSMIT_DEFER reason=send-failed ret=%d send=%d delay=%u seq=%u state=%u\n",
                                    defer_ret,
                                    send_ret,
                                    retry_delay_ms,
                                    retransmit->packet.seq,
                                    mesh_runtime.pending.state);
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        if (mesh_id_is_unicast(result->route_discovery_target_id) &&
            result->route_discovery_target_id != DEVICE_ID) {
            status_debug_printf("DBG_ROUTE_REQ_UPSTREAM_REPAIR target=0x%llx\n",
                                (unsigned long long)result->route_discovery_target_id);
            if (mesh_channel9_connection_count() == 0u) {
                mesh_schedule_async_route_request(result->route_discovery_target_id,
                                                  "rx-route-request-upstream");
            } else {
                mesh_schedule_route_waiting_retry_after("rx-route-request-ch9-wait",
                                                        MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            }
        } else {
            LOG_WRN("mesh route discovery needed after delivery failure");
        }
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);
        uint64_t route_ready_target_id = selected != NULL ? selected->gateway_id : GATEWAY_ID;
        uint64_t route_ready_next_hop_id = selected != NULL ? selected->next_hop_id : 0u;
        const struct app_mesh_route_ready_handoff_state route_ready_state = {
            .selected_route_valid = selected != NULL,
            .rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u,
            .deferred_peer_valid = mesh_id_is_unicast(mesh_route_ready_event_peer_id),
            .selected_peer_id = route_ready_next_hop_id,
            .deferred_peer_id = mesh_route_ready_event_peer_id,
        };
        struct app_mesh_route_ready_handoff_result route_ready_result;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_READY\n");
        }
        app_mesh_test_note_route_ready(route_ready_target_id,
                                       route_ready_next_hop_id,
                                       result->status);
        app_mesh_route_ready_handoff_on_ready(&route_ready_state,
                                              &route_ready_result);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_READY_DECISION sel=%u next=0x%llx rxq=%u def=0x%llx now=%u drain=%u waittx=%u prop=%u clear=%u\n",
                                route_ready_state.selected_route_valid ? 1u : 0u,
                                (unsigned long long)route_ready_state.selected_peer_id,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                (unsigned long long)mesh_route_ready_event_peer_id,
                                route_ready_result.propose_now ? 1u : 0u,
                                route_ready_result.schedule_rx_drain ? 1u : 0u,
                                route_ready_result.try_waiting_tx ? 1u : 0u,
                                route_ready_result.propose_deferred ? 1u : 0u,
                                route_ready_result.clear_route_reply_handoff ? 1u : 0u);
        }
        if (route_ready_result.clear_route_reply_handoff) {
            mesh_route_reply_handoff_clear("route-ready");
        }
        if (route_ready_result.clear_deferred_peer) {
            mesh_route_ready_event_peer_id = 0u;
        }
        LOG_INF("mesh reactive route ready");
        if (route_ready_result.propose_now) {
            int propose_ret;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_PROPOSE_NOW\n");
                LOG_INF("mesh route-ready to event-control gap: next=0x%016llx delay_ms=%u",
                        (unsigned long long)route_ready_result.peer_id,
                        MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
                k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            }
            propose_ret = mesh_propose_event_after_channel5_contact(route_ready_result.peer_id,
                                                                    "route-ready-event-propose");
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                app_mesh_route_ready_handoff_after_proposal(propose_ret,
                                                            &route_ready_result);
                if (route_ready_result.schedule_event_accept_wait) {
                    mesh_schedule_route_ready_event_retry(
                        &route_ready_result,
                        "route-ready-event-accept-wait");
                    return;
                }
            }
        }
        if (route_ready_result.schedule_rx_drain) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_RX_PENDING\n");
                mesh_route_ready_event_peer_id = route_ready_result.peer_id;
            }
            LOG_INF("mesh route ready deferred until queued RX control frames drain");
            mesh_schedule_route_waiting_retry("route-ready-rx-drain");
        } else if (route_ready_result.try_waiting_tx) {
            mesh_try_route_waiting_tx();
        }
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        int comm_ret = app_node_comm_note_gateway_confirmed(confirmed_packet);

        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        LOG_INF("mesh pending TX gateway acknowledged");
        if (comm_ret < 0 && comm_ret != -ENOENT && comm_ret != -ESTALE) {
            LOG_WRN("node communication gateway confirmation rejected: ret=%d",
                    comm_ret);
        }
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed != NULL) {
            mesh_report_callbacks->anchor_survey_delivery_gateway_confirmed(
                confirmed_packet);
        }
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) {
        LOG_INF("mesh pending TX hop progress acknowledged");
        (void)mesh_save_outbox_durable("hop-progress");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RELAY_BUSY) {
        LOG_INF("mesh pending TX deferred by relay busy hint");
        (void)mesh_save_outbox_durable("relay-busy");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_RETRY) {
        LOG_INF("mesh pending collection result scheduled for EACK retry");
        (void)mesh_save_outbox_durable("collection-retry");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_COLLECTION_CLOSED) {
        LOG_INF("mesh pending collection result stopped after collection close");
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_RESULT_GRANT_TERMINAL) {
        enum node_comm_terminal_reason reason =
            result->status == MESH_RELAY_ERR_RESULT_GRANT_DEADLINE_EXPIRED ?
                NODE_COMM_TERMINAL_DEADLINE_EXPIRED :
                result->status ==
                    MESH_RELAY_ERR_RESULT_GRANT_ATTEMPTS_EXHAUSTED ?
                NODE_COMM_TERMINAL_ATTEMPTS_EXHAUSTED :
                NODE_COMM_TERMINAL_PERMANENT_FAILURE;
        int comm_ret = app_node_comm_note_gateway_failed(
            &result->terminal.packet, reason);

        LOG_WRN("mesh result grant wait ended: status=%d reason=%u src=0x%016llx session=%u seq=%u",
                result->status,
                (unsigned int)reason,
                (unsigned long long)result->terminal.packet.src_id,
                result->terminal.packet.session_id,
                result->terminal.packet.seq);
        if (comm_ret < 0 && comm_ret != -ENOENT &&
            comm_ret != -EALREADY) {
            LOG_WRN("node communication result terminal rejected: ret=%d status=%d",
                    comm_ret, result->status);
        }
        if (mesh_report_callbacks != NULL &&
            mesh_report_callbacks->anchor_survey_delivery_transport_released !=
                NULL) {
            mesh_report_callbacks->anchor_survey_delivery_transport_released(
                &result->terminal.packet, false);
        }
        app_mesh_persistence_clear_outbox();
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (mesh_relay_result_bundle_pending(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        mesh_try_route_waiting_tx();
    }
}

static uint32_t mesh_drain_rx_queue_locked(const char *owner)
{
    struct mesh_rx_pending *pending = &mesh_rx_work_pending;
    struct mesh_relay_result *result = &mesh_work_result;
    uint32_t handled_count = 0u;
    int ret;

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_ENTER owner=%s q=%u\n",
                            owner == NULL ? "rx" : owner,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    while (k_msgq_get(&mesh_rx_msgq, pending, K_NO_WAIT) == 0) {
        uint32_t now_ms = k_uptime_get_32();
        bool handled_event_control = false;
        bool gateway_semantic_delivery_processed = false;
        struct proto_packet confirmed_packet = {0};
        bool confirmed_packet_valid = mesh_relay_tx_active(&mesh_runtime);

        if (confirmed_packet_valid) {
            confirmed_packet = mesh_runtime.pending.packet;
        }

        memset(result, 0, sizeof(*result));
        mesh_rx_pending_refresh_age(pending, now_ms);
        /* A peer TX slot can carry ACKs, telemetry, or control; ACK matching is opportunistic. */
        (void)mesh_ch9_tx_pending_handle_ack(&pending->packet,
                                             pending->payload,
                                             pending->payload_len,
                                             pending->previous_hop_id);
        if (mesh_gateway_route_test_should_reject_route_request(pending)) {
            continue;
        }
        ret = mesh_relay_handle_rx_with_random(&mesh_runtime,
                                               &pending->packet,
                                               pending->payload,
                                               pending->payload_len,
                                               pending->previous_hop_id,
                                               pending->link_quality,
                                               now_ms,
                                               sys_rand32_get(),
                                               result);
        if (ret != PROTO_OK) {
            LOG_WRN("mesh RX rejected: %d", ret);
            continue;
        }

        LOG_INF("mesh RX handled: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx actions=0x%08x status=%d",
                pending->packet.msg_type,
                (unsigned long long)pending->packet.src_id,
                (unsigned long long)pending->packet.dst_id,
                (unsigned long long)pending->previous_hop_id,
                result->actions,
                result->status);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            (pending->packet.msg_type == MSG_ROUTE_REQ ||
             pending->packet.msg_type == MSG_GATEWAY_ROUTE_ADV ||
             pending->packet.msg_type == MSG_ROUTE_REPLY ||
             pending->packet.msg_type == MSG_ROUTE_REPLY_ACK ||
             pending->packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
             pending->packet.msg_type == MSG_MESH_EVENT_ACCEPT)) {
            const struct route_candidate *selected =
                route_selected(&mesh_runtime.upstream);

            status_debug_printf("DBG_RX_HANDLE msg=0x%02x act=0x%08x st=%d q=%u sel=%u next=0x%llx hop=%u\n",
                                pending->packet.msg_type,
                                result->actions,
                                result->status,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                selected != NULL ? 1u : 0u,
                                (unsigned long long)(selected != NULL ?
                                    selected->next_hop_id : 0u),
                                selected != NULL ? selected->hop_count : 0u);
        }
        if (result->status == PROTO_ERR_NOT_FOUND &&
            pending->packet.dst_id != DEVICE_ID &&
            pending->packet.dst_id != MESH_BROADCAST_ID) {
            (void)mesh_request_route(pending->packet.dst_id, "rx-forward-miss");
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_packet_is_event_control_type(pending->packet.msg_type)) {
            handled_event_control = mesh_handle_event_control(&pending->packet,
                                                              pending->payload,
                                                              pending->payload_len,
                                                              pending->previous_hop_id,
                                                              pending->received_at_ms);
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            mesh_gateway_delivery_requires_semantic_acceptance(
                &pending->packet)) {
            int reservation_ret;
            int semantic_ret;

            /*
             * Reserve downstream host-output custody before mutating protocol
             * state.  A full BLE queue therefore leaves the radio packet
             * unaccepted and retryable instead of ACKing data that the host
             * can never observe.
             */
            mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
            gateway_semantic_delivery_processed = true;
            reservation_ret = gateway_ble_reserve_stream_packet(
                &pending->packet,
                pending->payload,
                pending->payload_len,
                pending->received_at_ms);
            if (reservation_ret != 1) {
                LOG_WRN("gateway semantic delivery deferred for BLE custody: msg=0x%02x src=0x%016llx ret=%d",
                        pending->packet.msg_type,
                        (unsigned long long)pending->packet.src_id,
                        reservation_ret);
            } else {
                semantic_ret = mesh_gateway_accept_semantic_delivery(pending);
                if (semantic_ret < 0) {
                    gateway_ble_cancel_stream_reservation();
                    LOG_WRN("gateway semantic delivery rejected without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            semantic_ret);
                } else if (semantic_ret ==
                           APP_GATEWAY_SEMANTIC_ACCEPT_NEW) {
                    ret = gateway_ble_commit_stream_reservation(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len);
                    if (ret != 1) {
                        gateway_ble_cancel_stream_reservation();
                        LOG_ERR("gateway semantic BLE custody commit failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                                pending->packet.msg_type,
                                (unsigned long long)pending->packet.src_id,
                                ret);
                        continue;
                    }
                    app_mesh_test_note_gateway_delivery(
                        &pending->packet,
                        pending->payload,
                        pending->payload_len,
                        pending->received_at_ms,
                        k_msgq_num_used_get(&mesh_rx_msgq));
                } else {
                    gateway_ble_cancel_stream_reservation();
                }

                ret = gateway_finalize_semantic_delivery(
                    &pending->packet,
                    pending->payload,
                    pending->payload_len,
                    pending->previous_hop_id,
                    pending->radio_channel,
                    pending->current_channel9_plan_valid ?
                    &pending->current_channel9_plan : NULL,
                    semantic_ret);
                if (ret < 0) {
                    LOG_WRN("gateway semantic post-custody finalization failed without ACK: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            ret);
                    continue;
                }
                ret = mesh_relay_commit_gateway_delivery(
                    &mesh_runtime,
                    &pending->packet,
                    pending->payload,
                    pending->payload_len,
                    pending->previous_hop_id,
                    k_uptime_get_32(),
                    result);
                if (ret != PROTO_OK) {
                    LOG_WRN("gateway semantic delivery commit failed: msg=0x%02x src=0x%016llx ret=%d",
                            pending->packet.msg_type,
                            (unsigned long long)pending->packet.src_id,
                            ret);
                }
            }
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            pending->packet.msg_type == MSG_GATEWAY_ACK) {
            if ((result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) != 0u) {
                status_debug_note("DBG_LEGACY_ACK_CONFIRMED\n");
            } else if (mesh_relay_tx_active(&mesh_runtime)) {
                status_debug_note("DBG_LEGACY_ACK_NO_MATCH\n");
            } else {
                status_debug_note("DBG_LEGACY_ACK_IDLE\n");
            }
        }
        mesh_handle_result_actions(result, pending->radio_channel, pending,
                                   confirmed_packet_valid ?
                                   &confirmed_packet : NULL);
        if (handled_event_control) {
            continue;
        }
        /*
         * Forwarding a broadcast can take long enough to materially advance
         * its protocol age. Keep the delivery timestamp and message age at
         * the same instant so local timing reconstruction does not restart
         * the survey after a relay operation.
         */
        mesh_rx_pending_refresh_age(pending, k_uptime_get_32());
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY &&
            !gateway_semantic_delivery_processed) {
            if (pending->packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending->packet,
                                            pending->payload,
                                            pending->payload_len,
                                            pending->previous_hop_id,
                                            pending->radio_channel,
                                            pending->current_channel9_plan_valid ?
                                            &pending->current_channel9_plan :
                                            NULL);
            } else if (pending->packet.msg_type == MSG_RESULT_BUNDLE) {
                gateway_note_command_result_bundle(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len,
                                                   pending->previous_hop_id,
                                                   pending->radio_channel,
                                                   pending->current_channel9_plan_valid ?
                                                   &pending->current_channel9_plan :
                                                   NULL);
            }
            mesh_report_gateway_handle_survey_discovery_report(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len,
                                                   pending->previous_hop_id,
                                                   pending->radio_channel,
                                                   pending->link_quality);
            app_mesh_test_note_gateway_delivery(&pending->packet,
                                                pending->payload,
                                                pending->payload_len,
                                                pending->received_at_ms,
                                                k_msgq_num_used_get(&mesh_rx_msgq));
            ret = gateway_ble_stream_packet(&pending->packet,
                                            pending->payload,
                                            pending->payload_len,
                                            pending->received_at_ms);
            if (ret < 0) {
                LOG_DBG("gateway BLE stream packet not queued: %d", ret);
            }
        } else if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
                   DEVICE_ROLE == ROLE_ANCHOR) {
            mesh_report_anchor_handle_local_command(&pending->packet,
                                                    pending->payload,
                                                    pending->payload_len);
            mesh_report_anchor_handle_survey_discovery_start(&pending->packet,
                                                 pending->payload,
                                                 pending->payload_len);
            mesh_report_anchor_handle_survey_pair_prepare(&pending->packet,
                                              pending->payload,
                                                    pending->payload_len);
        }
        handled_count++;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DRAIN_EXIT owner=%s handled=%u q=%u\n",
                            owner == NULL ? "rx" : owner,
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    return handled_count;
}

static void mesh_rx_handler_lock_note_owner(const char *owner)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_rx_handler_lock_owner = owner == NULL ? "rx" : owner;
    mesh_rx_handler_lock_since_ms = k_uptime_get_32();
}

static void mesh_rx_handler_lock_clear_owner(void)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    mesh_rx_handler_lock_owner = NULL;
    mesh_rx_handler_lock_since_ms = 0u;
}

static void mesh_rx_work_handler(struct k_work *work)
{
    uint32_t handled_count;
    int lock_ret;
    int submit_ret = 0;

    ARG_UNUSED(work);

    if (mesh_transport_paused()) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_WAIT q=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    lock_ret = k_mutex_lock(&mesh_rx_handler_lock, K_NO_WAIT);
    if (lock_ret != 0) {
        submit_ret = mesh_submit_work(&mesh_rx_work);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_WORK_LOCK_BUSY ret=%d q=%u owner=%s since=%u submit=%d\n",
                                lock_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                mesh_rx_handler_lock_owner == NULL ?
                                "unknown" : mesh_rx_handler_lock_owner,
                                mesh_rx_handler_lock_since_ms,
                                submit_ret);
        }
        return;
    }
    atomic_set(&mesh_rx_handler_active_state, 1);
    mesh_rx_handler_lock_note_owner("work");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_ENTER q=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    handled_count = mesh_drain_rx_queue_locked("work");
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_WORK_EXIT handled=%u q=%u\n",
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    atomic_set(&mesh_rx_handler_active_state, 0);
    mesh_rx_handler_lock_clear_owner();
    k_mutex_unlock(&mesh_rx_handler_lock);
}

bool mesh_process_queued_rx_now(const char *reason)
{
    uint32_t pending_count = k_msgq_num_used_get(&mesh_rx_msgq);
    uint32_t handled_count;
    struct app_mesh_coordinator_decision coordinator_decision;
    int lock_ret;
    int submit_ret;

    if (pending_count == 0u) {
        return false;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && radio_guard_uwb_busy()) {
        submit_ret = mesh_submit_work(&mesh_rx_work);
        status_debug_printf("DBG_MESH_RX_DIRECT_RF_BUSY submit=%d q=%u reason=%s\n",
                            submit_ret,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
        return false;
    }
    mesh_coordinator_decide_now(reason == NULL ? "queued-rx" : reason,
                                &coordinator_decision);
    if (!coordinator_decision.mesh_work_allowed) {
        submit_ret = mesh_submit_work(&mesh_rx_work);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_DIRECT_COORD_DEFER submit=%d q=%u state=%s reason=%s\n",
                                submit_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                app_mesh_coordinator_state_name(
                                    coordinator_decision.state),
                                reason == NULL ? "direct" : reason);
        }
        return false;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_WAIT q=%u reason=%s\n",
                            pending_count,
                            reason == NULL ? "direct" : reason);
    }
    lock_ret = k_mutex_lock(&mesh_rx_handler_lock, K_NO_WAIT);
    if (lock_ret != 0) {
        submit_ret = mesh_submit_work(&mesh_rx_work);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_MESH_RX_DIRECT_BUSY lock=%d submit=%d q=%u owner=%s since=%u reason=%s\n",
                                lock_ret,
                                submit_ret,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                mesh_rx_handler_lock_owner == NULL ?
                                "unknown" : mesh_rx_handler_lock_owner,
                                mesh_rx_handler_lock_since_ms,
                                reason == NULL ? "direct" : reason);
        }
        return false;
    }

    atomic_set(&mesh_rx_handler_active_state, 1);
    mesh_rx_handler_lock_note_owner(reason == NULL ? "direct" : reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_ENTER q=%u reason=%s\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
    }
    handled_count = mesh_drain_rx_queue_locked(reason == NULL ? "direct" : reason);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DIRECT_EXIT handled=%u q=%u reason=%s\n",
                            handled_count,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            reason == NULL ? "direct" : reason);
    }
    atomic_set(&mesh_rx_handler_active_state, 0);
    mesh_rx_handler_lock_clear_owner();
    k_mutex_unlock(&mesh_rx_handler_lock);

    if (k_msgq_num_used_get(&mesh_rx_msgq) > 0u) {
        (void)mesh_submit_work(&mesh_rx_work);
    }
    return handled_count > 0u;
}

static void mesh_handle_ch9_gateway_ack_timeout_route_failure(
    const struct mesh_outbound *failed,
    uint64_t timed_out_next_hop_id,
    uint8_t gateway_unacked,
    uint8_t requeued,
    uint32_t now_ms)
{
    const struct route_candidate *selected;
    enum route_delivery_action action;
    enum app_mesh_ch9_timeout_pressure_action pressure_action;
    uint64_t next_hop_id = 0u;
    uint32_t random_value = sys_rand32_get();
    uint32_t delay_ms = 0u;
    uint8_t failure_count = 0u;
    int select_ret = PROTO_ERR_NOT_FOUND;
    int route_ret = 0;

    if (failed == NULL || gateway_unacked == 0u) {
        return;
    }

    action = route_record_failure_at(&mesh_runtime.upstream,
                                     ROUTE_FAILURE_GATEWAY_ACK,
                                     now_ms);
    pressure_action = app_mesh_ch9_timeout_pressure_decide(
        failed,
        DEVICE_ROLE == ROLE_ANCHOR,
        mesh_runtime_has_transit_reservation(),
        false,
        DEVICE_ID);
    if (action != ROUTE_DELIVERY_DISCOVER) {
        select_ret = mesh_relay_select_next_hop(&mesh_runtime,
                                                failed->packet.dst_id,
                                                &next_hop_id);
    }
    if (pressure_action == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
        if (action != ROUTE_DELIVERY_RETRY_CURRENT) {
            mesh_relay_clear_channel9_timing(&mesh_runtime,
                                             timed_out_next_hop_id);
        }
        if (action == ROUTE_DELIVERY_DISCOVER || select_ret != PROTO_OK) {
            mesh_relay_invalidate_upstream_route(&mesh_runtime);
        }
    } else if (action == ROUTE_DELIVERY_RETRY_CURRENT &&
               select_ret == PROTO_OK) {
        selected = route_selected(&mesh_runtime.upstream);
        failure_count = selected == NULL ? 1u : selected->failure_count;
        delay_ms = mesh_relay_retry_backoff_ms(failure_count, random_value);
        report_tx_schedule_backoff(delay_ms, "ch9-gateway-ack-timeout");
    } else if (action == ROUTE_DELIVERY_TRY_ALTERNATE && select_ret == PROTO_OK) {
        report_tx_schedule(0u);
    } else if (pressure_action == APP_MESH_CH9_TIMEOUT_DEFER_LOCAL) {
        delay_ms = ROUTE_PARENT_HOLDDOWN_MS;
        route_ret = -EAGAIN;
        report_tx_schedule_backoff(delay_ms,
                                   "ch9-gateway-ack-downstream-reserved");
    } else {
        mesh_relay_invalidate_upstream_route(&mesh_runtime);
        if (timed_out_next_hop_id == GATEWAY_ID) {
            mesh_direct_gateway_bulk_probe_suppress(now_ms,
                                                    "ch9-gateway-ack-timeout");
        }
        route_ret = mesh_request_route(failed->packet.dst_id,
                                       "ch9-gateway-ack-timeout");
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_ROUTE_FAIL action=%s pressure=%u src=0x%llx type=0x%02x gw_unacked=%u requeued=%u fail=%u delay=%u route_ret=%d select=%d next=0x%llx timed=0x%llx\n",
                            route_delivery_action_name(action),
                            pressure_action,
                            (unsigned long long)failed->packet.src_id,
                            failed->packet.msg_type,
                            gateway_unacked,
                            requeued,
                            failure_count,
                            delay_ms,
                            route_ret,
                            select_ret,
                            (unsigned long long)next_hop_id,
                            (unsigned long long)timed_out_next_hop_id);
    }
}

static void mesh_ch9_tx_pending_handle_timeout(uint32_t now_ms)
{
    struct mesh_outbound route_failure_packet = {0};
    uint64_t timed_out_next_hop_id = 0u;
    uint8_t requeued = 0u;
    uint8_t acked = 0u;
    uint8_t gateway_unacked = 0u;
    uint8_t transit_dropped = 0u;
    bool route_failure_packet_valid = false;

    if (!mesh_ch9_tx_pending.active ||
        !uptime_deadline_reached(now_ms, mesh_ch9_tx_pending.deadline_ms)) {
        return;
    }

    timed_out_next_hop_id = mesh_ch9_tx_pending.next_hop_id;
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        struct mesh_ch9_tx_pending_entry *entry = &mesh_ch9_tx_pending.entries[i];

        if (entry->acked) {
            acked++;
            continue;
        }
        struct mesh_outbound clean = entry->outbound;
        int strip_ret = mesh_outbound_clear_ch9_batch_metadata(&clean);
        const struct mesh_outbound *retry =
            strip_ret == PROTO_OK ? &clean : &entry->outbound;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && strip_ret != PROTO_OK) {
            status_debug_printf("DBG_CH9_TX_ACK_TIMEOUT_STRIP_FAIL ret=%d seq=%u\n",
                                strip_ret,
                                entry->outbound.packet.seq);
        }
        if (app_mesh_ch9_tx_timeout_counts_route_failure(retry,
                                                         timed_out_next_hop_id,
                                                         GATEWAY_ID)) {
            gateway_unacked++;
            if (!route_failure_packet_valid) {
                route_failure_packet = *retry;
                route_failure_packet_valid = true;
            }
        }
        if (app_mesh_ch9_timeout_pressure_decide(
                retry,
                DEVICE_ROLE == ROLE_ANCHOR,
                mesh_runtime_has_transit_reservation(),
                false,
                DEVICE_ID) == APP_MESH_CH9_TIMEOUT_DROP_TRANSIT) {
            entry->acked = true;
            transit_dropped++;
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
            status_debug_printf("DBG_TRANSIT_CUSTODY_DROP src=0x%llx seq=%u type=0x%02x reason=gateway-ack-timeout\n",
                                (unsigned long long)retry->packet.src_id,
                                retry->packet.seq,
                                retry->packet.msg_type);
            continue;
        }
    }

    requeued = mesh_ch9_tx_pending_requeue_unacked(now_ms);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_TX_ACK_TIMEOUT\n");
        status_debug_printf("DBG_CH9_TX_ACK_TIMEOUT n=%u acked=%u dropped=%u requeued=%u retained=%u now=%u deadline=%u\n",
                            (uint8_t)(acked + transit_dropped + requeued +
                                      mesh_ch9_tx_pending.count),
                            acked,
                            transit_dropped,
                            requeued,
                            mesh_ch9_tx_pending.count,
                            now_ms,
                            mesh_ch9_tx_pending.deadline_ms);
    }
    LOG_WRN("mesh channel-9 TX batch ACK timeout: acked=%u dropped=%u requeued=%u retained=%u",
            acked,
            transit_dropped,
            requeued,
            mesh_ch9_tx_pending.count);
    if (route_failure_packet_valid) {
        mesh_handle_ch9_gateway_ack_timeout_route_failure(&route_failure_packet,
                                                          timed_out_next_hop_id,
                                                          gateway_unacked,
                                                          requeued,
                                                          now_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint8_t skipped;

        status_debug_note("DBG_CH9_TX_ACK_TIMEOUT_REQUEUE\n");
        skipped = mesh_advance_channel9_timing_past(timed_out_next_hop_id,
                                                    now_ms,
                                                    "ack-timeout");
        (void)mesh_expire_channel9_timings(now_ms, "ch9-ack-timeout");
        status_debug_printf("DBG_CH9_TX_ACK_REQUEUE next=0x%llx skip=%u\n",
                            (unsigned long long)timed_out_next_hop_id,
                            skipped);
    }
    if (!mesh_ch9_tx_pending.active) {
        mesh_ch9_tx_pending_clear();
    } else {
        mesh_schedule_tx_timeout();
    }
    if (requeued > 0u || report_tx_queue_used() > 0u) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_TX_ACK_REQUEUE_SCHED requeued=%u q=%u\n",
                                requeued,
                                report_tx_queue_used());
        }
        report_tx_schedule(0u);
    }
}

static bool mesh_defer_due_retry_to_channel9_tx_slot(uint32_t now_ms)
{
    uint64_t peer_id;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        mesh_runtime.pending.state != MESH_RELAY_TX_WAIT_RETRY_BACKOFF ||
        mesh_runtime.pending.radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        !uptime_deadline_reached(now_ms, mesh_runtime.pending.retry_after_ms)) {
        return false;
    }

    peer_id = mesh_runtime.pending.next_hop_id;
    (void)mesh_advance_channel9_timing_past(peer_id,
                                            now_ms,
                                            "retry-slot-preflight");
    (void)mesh_expire_channel9_timings(now_ms, "retry-slot-preflight");
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];
        uint32_t prepare_ms;
        uint32_t delay_ms;

        if (!entry->valid || entry->next_hop_id != peer_id ||
            !mesh_event_timing_usable(&entry->timing, now_ms) ||
            !app_mesh_ch9_retry_next_local_tx_prepare_ms(
                &entry->timing,
                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS,
                &prepare_ms)) {
            continue;
        }

        delay_ms = uptime_ms_until_deadline(now_ms, prepare_ms);
        if (delay_ms == 0u) {
            delay_ms = 1u;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH9_RETRY_DEFER_RX_SLOT peer=0x%llx cnt=%u now=%u prepare=%u delay=%u\n",
                                (unsigned long long)peer_id,
                                entry->timing.event_counter,
                                now_ms,
                                prepare_ms,
                                delay_ms);
        }
        (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, delay_ms);
        mesh_schedule_uwb_rx(0u);
        return true;
    }
    return false;
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result *result = &mesh_work_result;
    struct mesh_outbound *pending_waiting = &mesh_tx_timeout_pending_waiting;
    bool pending_route_waiting = false;
#if DEVICE_ROLE == ROLE_ANCHOR
    struct mesh_outbound *pending_report = &mesh_tx_timeout_pending_report;
    bool pending_anchor_report = false;
#endif

    ARG_UNUSED(work);
    if (mesh_transport_paused()) {
        return;
    }
    memset(result, 0, sizeof(*result));
    memset(pending_waiting, 0, sizeof(*pending_waiting));
#if DEVICE_ROLE == ROLE_ANCHOR
    memset(pending_report, 0, sizeof(*pending_report));
#endif

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_TX_TIMEOUT_ENTER relay=%u route=%u ch9pend=%u bundle=%u now=%u\n",
                            mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                            mesh_route_waiting_tx_valid ? 1u : 0u,
                            mesh_ch9_tx_pending.active ? 1u : 0u,
                            mesh_relay_result_bundle_pending(&mesh_runtime) ? 1u : 0u,
                            k_uptime_get_32());
    }

    mesh_ch9_tx_pending_handle_timeout(k_uptime_get_32());
    if (mesh_defer_due_retry_to_channel9_tx_slot(k_uptime_get_32())) {
        return;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) {
        (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, REPORT_TX_RETRY_DELAY_MS);
        return;
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    if (mesh_relay_tx_active(&mesh_runtime) &&
        mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
        mesh_runtime.pending.packet.src_id == DEVICE_ID) {
        pending_report->packet = mesh_runtime.pending.packet;
        pending_report->payload_len = mesh_runtime.pending.payload_len;
        if (pending_report->payload_len > 0u) {
            memcpy(pending_report->payload,
                   mesh_runtime.pending.payload,
                   pending_report->payload_len);
        }
        pending_anchor_report = true;
    }
#endif

    if (mesh_relay_tx_active(&mesh_runtime)) {
        pending_waiting->packet = mesh_runtime.pending.packet;
        pending_waiting->payload_len = mesh_runtime.pending.payload_len;
        pending_waiting->radio_channel = mesh_runtime.pending.radio_channel;
        pending_waiting->next_hop_id = mesh_runtime.pending.next_hop_id;
        if (pending_waiting->payload_len > 0u) {
            memcpy(pending_waiting->payload,
                   mesh_runtime.pending.payload,
                   pending_waiting->payload_len);
        }
        pending_route_waiting = mesh_tx_can_wait_for_route(pending_waiting);
    }

    if (mesh_relay_tick_with_random(&mesh_runtime,
                                    k_uptime_get_32(),
                                    sys_rand32_get(),
                                    result) != PROTO_OK) {
        return;
    }
    mesh_handle_result_actions(result, UWB_CHANNEL_WAKE_CONTACT, NULL, NULL);
    if (mesh_relay_tx_active(&mesh_runtime) ||
        mesh_relay_result_bundle_pending(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }

    if (pending_route_waiting &&
        (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        int route_ret;

        if (!mesh_relay_tx_active(&mesh_runtime)) {
            mesh_store_route_waiting_tx(pending_waiting);
        }
        route_ret = mesh_request_route(pending_waiting->packet.dst_id, "pending-tx-timeout");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_TX_TIMEOUT_ROUTE_REQ ret=%d relay=%u route=%u dst=0x%llx\n",
                                route_ret,
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                (unsigned long long)pending_waiting->packet.dst_id);
        }
        if (route_ret == -ETIMEDOUT && !mesh_relay_tx_active(&mesh_runtime)) {
            mesh_schedule_route_waiting_exhausted_retry(
                "pending-tx-route-exhausted");
        }
    }

#if DEVICE_ROLE == ROLE_ANCHOR
    if (pending_anchor_report &&
        (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        LOG_WRN("requeueing click report after mesh route loss");
        (void)queue_anchor_report(pending_report);
    }
#endif
}

static bool mesh_queue_from_frame_at_internal(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t link_quality,
    uint8_t radio_channel,
    uint32_t received_at_ms,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t current_channel9_peer_id,
    bool submit_work,
    bool *valid_mesh_frame,
    uint64_t *previous_hop_id)
{
    struct mesh_frame_parse_context context = {0};
    struct mesh_rx_pending pending = {0};
    int ret;

    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = false;
    }
    if (frame == NULL || frame_len == 0u) {
        return false;
    }
    if (uwb_mesh_frame_decode(frame,
                              frame_len,
                              NETWORK_ID,
                              DEVICE_ID,
                              &context.previous_hop_id,
                              &context.packet,
                              context.payload,
                              sizeof(context.payload),
                              &context.payload_len) != PROTO_OK) {
        if (mesh_gateway_route_test_role()) {
            status_debug_printf("DBG_GATEWAY_RX_DECODE_FAIL ch=%u len=%u q=%u\n",
                                radio_channel,
                                (unsigned int)frame_len,
                                link_quality);
        }
        return false;
    }
    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = true;
    }
    if (previous_hop_id != NULL) {
        *previous_hop_id = context.previous_hop_id;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_DECODE ch=%u msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx seq=%u flags=0x%02x plen=%u q=%u\n",
                            radio_channel,
                            context.packet.msg_type,
                            (unsigned long long)context.packet.src_id,
                            (unsigned long long)context.packet.dst_id,
                            (unsigned long long)context.previous_hop_id,
                            context.packet.seq,
                            context.packet.flags,
                            (unsigned int)context.payload_len,
                            link_quality);
    }

    pending.packet = context.packet;
    if (context.payload_len > 0u) {
        memcpy(pending.payload, context.payload, context.payload_len);
    }
    pending.payload_len = (uint16_t)context.payload_len;
    pending.previous_hop_id = context.previous_hop_id;
    pending.link_quality = link_quality;
    pending.radio_channel = radio_channel;
    pending.received_at_ms = received_at_ms;
    if (radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
        current_channel9_plan != NULL &&
        context.previous_hop_id == current_channel9_peer_id &&
        !uptime_deadline_reached(received_at_ms, current_channel9_plan->end_ms)) {
        pending.current_channel9_plan_valid = true;
        pending.current_channel9_plan = *current_channel9_plan;
    }
    if (app_mesh_rx_policy_should_drop(IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER),
                                       pending.packet.msg_type)) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_ROUTE_ADV_IGNORED_TX prev=0x%llx seq=%u\n",
                                (unsigned long long)pending.previous_hop_id,
                                pending.packet.seq);
        }
        return false;
    }
    app_mesh_test_note_wake_event(&pending.packet,
                                  pending.previous_hop_id,
                                  pending.link_quality,
                                  radio_channel);
    mesh_note_c5_control_rx(&pending.packet,
                            pending.previous_hop_id,
                            radio_channel,
                            "c5-control-rx");
    status_debug_uwb_rx_channel_pulse(radio_channel);

    ret = k_msgq_put(&mesh_rx_msgq, &pending, K_NO_WAIT);
    if (ret < 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        LOG_WRN("mesh UWB RX queue full; dropped msg=0x%02x src=0x%016llx dst=0x%016llx",
                pending.packet.msg_type,
                (unsigned long long)pending.packet.src_id,
                (unsigned long long)pending.packet.dst_id);
        return false;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        uwb_anchor_note_mesh_packet(&anchor_uwb_session);
    }
    HIGH_DEBUG_COUNTER_INC(mesh_rx);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_QUEUED ch=%u msg=0x%02x src=0x%llx dst=0x%llx prev=0x%llx seq=%u q=%u\n",
                            radio_channel,
                            pending.packet.msg_type,
                            (unsigned long long)pending.packet.src_id,
                            (unsigned long long)pending.packet.dst_id,
                            (unsigned long long)pending.previous_hop_id,
                            pending.packet.seq,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        pending.packet.msg_type == MSG_GATEWAY_ROUTE_ADV) {
        status_debug_printf("DBG_ROUTE_ADV_QUEUED q=%u prev=0x%llx seq=%u\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            (unsigned long long)pending.previous_hop_id,
                            pending.packet.seq);
    }
    if (pending.packet.msg_type == MSG_GATEWAY_ACK) {
        status_debug_tx_gateway_ack_rx_pulse();
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_GATEWAY_ACK_RX\n");
        }
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        high_debug_log_event("GATEWAY_ACK_RX",
                             "src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                             (unsigned long long)pending.packet.src_id,
                             (unsigned long long)pending.packet.dst_id,
                             (unsigned long long)pending.previous_hop_id,
                             pending.packet.seq,
                             pending.link_quality);
    }
    high_debug_log_event("MESH_RX",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u queue_depth=%u",
                         pending.packet.msg_type,
                         (unsigned long long)pending.packet.src_id,
                         (unsigned long long)pending.packet.dst_id,
                         (unsigned long long)pending.previous_hop_id,
                         pending.packet.seq,
                         pending.link_quality,
                         k_msgq_num_used_get(&mesh_rx_msgq));
    LOG_INF("mesh UWB RX queued: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u queue_depth=%u role=%s",
            pending.packet.msg_type,
            (unsigned long long)pending.packet.src_id,
            (unsigned long long)pending.packet.dst_id,
            (unsigned long long)pending.previous_hop_id,
            pending.link_quality,
            k_msgq_num_used_get(&mesh_rx_msgq),
            role_name());

    if (submit_work) {
        (void)mesh_submit_work(&mesh_rx_work);
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_DEFERRED q=%u msg=0x%02x\n",
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            pending.packet.msg_type);
    }
    return true;
}

static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
                                     const struct mesh_event_plan *current_channel9_plan,
                                     uint64_t current_channel9_peer_id,
                                     bool *valid_mesh_frame,
                                     uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at_internal(frame,
                                             frame_len,
                                             link_quality,
                                             radio_channel,
                                             received_at_ms,
                                             current_channel9_plan,
                                             current_channel9_peer_id,
                                             true,
                                             valid_mesh_frame,
                                             previous_hop_id);
}

bool mesh_queue_from_frame(const uint8_t *frame,
                           size_t frame_len,
                           uint8_t link_quality,
                           uint8_t radio_channel,
                           bool *valid_mesh_frame,
                           uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at(frame,
                                    frame_len,
                                    link_quality,
                                    radio_channel,
                                    k_uptime_get_32(),
                                    NULL,
                                    0u,
                                    valid_mesh_frame,
                                    previous_hop_id);
}

bool mesh_queue_from_frame_deferred(const uint8_t *frame,
                                    size_t frame_len,
                                    uint8_t link_quality,
                                    uint8_t radio_channel,
                                    bool *valid_mesh_frame,
                                    uint64_t *previous_hop_id)
{
    return mesh_queue_from_frame_at_internal(frame,
                                             frame_len,
                                             link_quality,
                                             radio_channel,
                                             k_uptime_get_32(),
                                             NULL,
                                             0u,
                                             false,
                                             valid_mesh_frame,
                                             previous_hop_id);
}

void mesh_submit_queued_rx(void)
{
    int ret = mesh_submit_work(&mesh_rx_work);

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_MESH_RX_SUBMIT ret=%d q=%u routeq=1\n",
                            ret,
                            k_msgq_num_used_get(&mesh_rx_msgq));
    }
}

uint32_t mesh_rx_pending_count(void)
{
    return k_msgq_num_used_get(&mesh_rx_msgq);
}

bool mesh_rx_response_active(void)
{
    return atomic_get(&mesh_rx_response_active_state) != 0;
}

bool mesh_anchor_low_duty_scan_should_defer(uint32_t *retry_ms)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t selected_delay_ms = 0u;
    uint32_t min_gap_ms;
    bool found = false;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR) {
        return false;
    }

    min_gap_ms = ANCHOR_UWB_SCAN_RX_MS + MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS;
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry = &mesh_runtime.event_timings[i];
        struct mesh_event_timing timing = {0};
        uint32_t delay_ms;

        if (!mesh_channel9_next_required_activity(entry, &timing) ||
            !mesh_event_timing_usable(&timing, now_ms)) {
            continue;
        }
        delay_ms = uptime_ms_until_deadline(now_ms,
                                            mesh_channel9_prepare_start_ms(&timing));
        if (!found || delay_ms < selected_delay_ms) {
            selected_delay_ms = delay_ms;
            found = true;
        }
    }
    if (!found || selected_delay_ms > min_gap_ms) {
        return false;
    }

    if (retry_ms != NULL) {
        *retry_ms = selected_delay_ms +
                    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS +
                    MESH_EVENT_DEFAULT_WINDOW_MS +
                    MESH_EVENT_RX_LATE_GUARD_MS +
                    ANCHOR_UWB_SCAN_BUSY_RETRY_MS;
    }
    return true;
}

static bool mesh_decode_channel5_wake_claim(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim,
    bool *embedded_route_frame_out)
{
    bool embedded_route_frame = false;
    int ret;

    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = false;
    }
    if (frame == NULL || frame_len == 0u || claim == NULL) {
        return false;
    }

    if (frame_len == UWB_WAKE_CLAIM_LEN) {
        ret = uwb_decode_wake_claim(frame, frame_len, claim);
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
               frame_len > UWB_WAKE_CLAIM_LEN) {
        ret = uwb_decode_wake_claim(frame, UWB_WAKE_CLAIM_LEN, claim);
        embedded_route_frame = ret == PROTO_OK;
    } else {
        ret = PROTO_ERR_MALFORMED;
    }
    if (ret != PROTO_OK) {
        return false;
    }
    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = embedded_route_frame;
    }
    return true;
}

static bool mesh_frame_requires_anchor_click_handoff(const uint8_t *frame,
                                                     size_t frame_len,
                                                     struct uwb_wake_claim_frame *claim)
{
    bool local_can_range_clicks =
        DEVICE_ROLE == ROLE_ANCHOR &&
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);

    return mesh_decode_channel5_wake_claim(frame, frame_len, claim, NULL) &&
           app_mesh_c5_wake_claim_requires_anchor_handoff(
               claim->flags, local_can_range_clicks);
}

static bool mesh_handoff_anchor_click_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t quality,
    uint32_t observed_packet_ms)
{
    bool handled = false;

    if (claim == NULL) {
        return false;
    }

    anchor_click_window_set_active(true);
    if (mesh_preempt_for_click_event() < 0) {
        anchor_click_window_set_active(false);
        LOG_ERR("anchor click wake handoff deferred because mesh custody preemption failed");
        return false;
    }
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->anchor_handle_click_wake_claim != NULL) {
        handled = mesh_report_callbacks->anchor_handle_click_wake_claim(
            claim,
            quality,
            observed_packet_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_ANCHOR_CLICK_HANDOFF evt=%u attempt=%u handled=%u q=%u\n",
                            claim->click_event_id,
                            claim->attempt_index,
                            handled ? 1u : 0u,
                            quality);
    }
    if (!handled) {
        LOG_ERR("anchor click wake handoff failed: clicker=0x%016llx event_seq=%u attempt=%u; claim not reclassified as route traffic",
                (unsigned long long)claim->clicker_id,
                claim->click_event_id,
                claim->attempt_index);
    }
    return handled;
}

static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t link_quality,
                                            bool *embedded_route_frame_out,
                                            bool *click_priority_out)
{
    struct uwb_wake_claim_frame claim;
    uint32_t contact_expires_at_ms;
    bool embedded_route_frame = false;

    if (click_priority_out != NULL) {
        *click_priority_out = false;
    }
    if (!mesh_decode_channel5_wake_claim(frame,
                                         frame_len,
                                         &claim,
                                         &embedded_route_frame)) {
        return false;
    }

    if (app_mesh_c5_wake_claim_preempts_mesh(claim.flags) &&
        !app_mesh_c5_wake_claim_requires_anchor_handoff(
            claim.flags,
            DEVICE_ROLE == ROLE_ANCHOR &&
            !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER))) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
            status_debug_printf("DBG_TX_CLICK_CLAIM_IGNORED src=0x%llx evt=%u attempt=%u\n",
                                (unsigned long long)claim.clicker_id,
                                claim.click_event_id,
                                claim.attempt_index);
        }
        return false;
    }

    app_mesh_test_note_wake_claim(claim.clicker_id,
                                  claim.click_event_id,
                                  claim.attempt_index,
                                  link_quality);
    if (click_priority_out != NULL) {
        *click_priority_out =
            app_mesh_c5_wake_claim_preempts_mesh(claim.flags);
    }
    mesh_gateway_route_test_note_channel5_contact(claim.clicker_id, "wake-claim");
    contact_expires_at_ms = k_uptime_get_32() + claim.claimed_duration_ms;
    if (DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        contact_expires_at_ms += MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
    }
    mesh_c5_contact_accept(claim.clicker_id,
                           C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                           contact_expires_at_ms,
                           embedded_route_frame ? "embedded-wake-claim" :
                                                  "wake-claim");
    if (DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        const struct app_mesh_c5_route_adv_timing timing = {
            .wake_to_route_delay_ms = MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS,
            .request_flood_burst_ms = FLOOD_RELAY_BURST_MS,
            .embedded_reply_guard_ms = MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS,
            .route_adv_reply_guard_ms = MESH_ROUTE_TEST_ROUTE_ADV_REPLY_GUARD_MS,
        };
        uint32_t route_adv_delay_ms =
            app_mesh_c5_route_adv_response_delay_ms(
                claim.wake_train_ends_in_ms,
                embedded_route_frame,
                &timing);

        status_debug_printf("DBG_GW_ADV_WAKE_DELAY wake=%u claimed=%u delay=%u embed=%u exp=%u\n",
                            claim.wake_train_ends_in_ms,
                            claim.claimed_duration_ms,
                            route_adv_delay_ms,
                            embedded_route_frame ? 1u : 0u,
                            contact_expires_at_ms);
        (void)app_node_comm_request_route_refresh(
            route_adv_delay_ms, "route-solicit-wake-claim", false);
    }
    high_debug_log_event("MESH_CH5_WAKE_RX",
                         "src=0x%016llx event_seq=%u attempt=%u quality=%u role=%s",
                         (unsigned long long)claim.clicker_id,
                         claim.click_event_id,
                         claim.attempt_index,
                         link_quality,
                         role_name());
    LOG_INF("mesh channel-5 wake claim RX: src=0x%016llx event_seq=%u attempt=%u quality=%u role=%s",
            (unsigned long long)claim.clicker_id,
            claim.click_event_id,
            claim.attempt_index,
            link_quality,
            role_name());
    if (embedded_route_frame) {
        (void)mesh_queue_embedded_route_request(&claim,
                                                &frame[UWB_WAKE_CLAIM_LEN],
                                                frame_len - UWB_WAKE_CLAIM_LEN,
                                                link_quality);
    }
    if (embedded_route_frame_out != NULL) {
        *embedded_route_frame_out = embedded_route_frame;
    }
    return true;
}

bool mesh_anchor_handoff_route_wake_frame(const uint8_t *frame,
                                          size_t frame_len,
                                          uint8_t link_quality)
{
    struct uwb_wake_claim_frame claim;
    bool embedded_route_frame = false;
    bool captured_route_reply = false;
    uint32_t listen_ms;
    int listen_ret = 0;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE != ROLE_ANCHOR ||
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ||
        !mesh_decode_channel5_wake_claim(frame,
                                         frame_len,
                                         &claim,
                                         &embedded_route_frame) ||
        app_mesh_c5_wake_claim_requires_anchor_handoff(claim.flags, true)) {
        return false;
    }

    if (!mesh_handle_channel5_wake_claim(frame,
                                         frame_len,
                                         link_quality,
                                         &embedded_route_frame,
                                         NULL)) {
        return false;
    }

    listen_ms = claim.claimed_duration_ms;
    if (listen_ms < MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS) {
        listen_ms = MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS;
    }
    if (listen_ms > WAKE_ADV_MS + MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS) {
        listen_ms = WAKE_ADV_MS + MESH_ROUTE_TEST_POST_WAKE_ROUTE_RX_MS;
    }

    status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_HANDOFF src=0x%llx evt=%u attempt=%u flags=0x%02x embedded=%u listen=%u\n",
                        (unsigned long long)claim.clicker_id,
                        claim.click_event_id,
                        claim.attempt_index,
                        claim.flags,
                        embedded_route_frame ? 1u : 0u,
                        listen_ms);

    if (!embedded_route_frame) {
        const char *followup_reason =
            app_mesh_c5_wake_followup_is_control(claim.flags) ?
            "gateway-command-wake-followup" :
            "anchor-route-wake-followup";

        listen_ret = mesh_listen_for_route_reply(claim.clicker_id,
                                                 followup_reason,
                                                 listen_ms,
                                                 NULL,
                                                 &captured_route_reply);
    }
    if (mesh_rx_pending_count() > 0u) {
        (void)mesh_process_queued_rx_now("anchor-route-wake-followup");
    }
    status_debug_printf("DBG_ANCHOR_ROUTE_WAKE_HANDOFF_DONE src=0x%llx embedded=%u ret=%d pending=%u\n",
                        (unsigned long long)claim.clicker_id,
                        embedded_route_frame ? 1u : 0u,
                        listen_ret,
                        mesh_rx_pending_count());
    return true;
}

static bool mesh_process_received_frame(const uint8_t *frame,
                                        size_t frame_len,
                                        uint8_t quality,
                                        bool channel9_event,
                                        uint64_t channel9_peer_id,
                                        const struct mesh_event_plan *channel9_plan,
                                        uint32_t observed_packet_ms,
                                        bool *channel9_peer_observed)
{
    bool valid_mesh_frame = false;
    uint64_t rx_previous_hop_id = 0u;

    if (channel9_peer_observed != NULL) {
        *channel9_peer_observed = false;
    }

    if (!channel9_event) {
        struct uwb_wake_claim_frame claim;

        if (mesh_frame_requires_anchor_click_handoff(frame,
                                                     frame_len,
                                                     &claim)) {
            (void)mesh_handoff_anchor_click_claim(&claim,
                                                  quality,
                                                  observed_packet_ms);
            return true;
        }
        if (mesh_handle_channel5_wake_claim(frame,
                                            frame_len,
                                            quality,
                                            NULL,
                                            NULL)) {
            LOG_DBG("mesh UWB RX accepted channel-5 wake claim: len=%u",
                    (unsigned int)frame_len);
            return true;
        }
    }

    if (mesh_queue_from_frame_at(frame,
                                 frame_len,
                                 quality,
                                 channel9_event ? UWB_CHANNEL_MESH_PAYLOAD :
                                                  UWB_CHANNEL_WAKE_CONTACT,
                                 observed_packet_ms,
                                 channel9_event ? channel9_plan : NULL,
                                 channel9_peer_id,
                                 &valid_mesh_frame,
                                 &rx_previous_hop_id)) {
        if (channel9_event &&
            channel9_plan != NULL &&
            rx_previous_hop_id == channel9_peer_id) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                int32_t delta_ms = (int32_t)(observed_packet_ms - channel9_plan->start_ms);

                status_debug_printf("DBG_CH9_RX_EXPECTED delta=%d q=%u\n",
                                    delta_ms,
                                    quality);
            }
            mesh_relay_note_channel9_rx(&mesh_runtime,
                                        rx_previous_hop_id,
                                        channel9_plan->start_ms,
                                        observed_packet_ms);
            if (channel9_peer_observed != NULL) {
                *channel9_peer_observed = true;
            }
        } else if (channel9_event &&
                   channel9_plan != NULL &&
                   IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_RX_UNEXPECTED_PEER\n");
            status_debug_printf("DBG_CH9_RX_UNEXPECTED prev=0x%llx exp=0x%llx\n",
                                (unsigned long long)rx_previous_hop_id,
                                (unsigned long long)channel9_peer_id);
        }
        LOG_DBG("mesh UWB RX frame accepted: len=%u", (unsigned int)frame_len);
        return true;
    }

    if (!valid_mesh_frame) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
            status_debug_printf("DBG_RX_REJECT mode=%u len=%u q=%u\n",
                                channel9_event ? 9u : 5u,
                                (unsigned int)frame_len,
                                quality);
        }
        LOG_DBG("mesh UWB RX ignored non-mesh frame: len=%u quality=%u",
                (unsigned int)frame_len,
                quality);
    }

    return false;
}

static void mesh_uwb_rx_work_handler(struct k_work *work)
{
    uint8_t *frame = mesh_uwb_rx_frame;
    size_t frame_len = 0u;
    uint8_t quality = 0u;
    uint64_t channel9_peer_id = 0u;
    struct mesh_event_plan channel9_plan = {0};
    uint32_t window_ms;
    uint32_t observed_packet_ms = 0u;
    uint32_t channel9_rx_armed_ms = 0u;
    uint8_t channel9_timing_index = 0u;
    uint8_t channel9_frames_seen = 0u;
    int64_t uwb_window_start_ms = -1;
    enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
    bool channel9_event = false;
    bool channel5_gap_scan = false;
    bool gateway_route_preempt = false;
    bool gateway_continuous_ch5 = false;
    bool frame_processed_inline = false;
    bool channel9_peer_observed = false;
    uint16_t gateway_ch9_recoverable_rx_errors = 0u;
    uint32_t channel5_gap_window_ms = 0u;
    uint32_t channel5_gap_deadline_ms = 0u;
    uint16_t channel5_gap_frames_seen = 0u;
    int channel5_gap_last_error = 0;
    uint32_t route_adv_wait_ms = 0u;
    struct app_mesh_coordinator_decision coordinator_decision;
    int ret;

    ARG_UNUSED(work);
    if (mesh_transport_paused() ||
        !mesh_rx_handoff_scan_rearm_allowed()) {
        mesh_uwb_rx_active = false;
        return;
    }
    app_watchdog_note_radio_progress();

    if (mesh_gateway_route_test_role()) {
        uint32_t now_ms = k_uptime_get_32();

        if (gateway_rx_diag_next_ms == 0u ||
            uptime_deadline_reached(now_ms, gateway_rx_diag_next_ms)) {
            gateway_rx_diag_next_ms = now_ms + 1000u;
            status_debug_printf("DBG_GATEWAY_UWB_RX_WORK now=%u active=%u qrx=%u qtx=%u relay=%u wait=%u ack=%u adv_due=%u conn=%u\n",
                                now_ms,
                                mesh_uwb_rx_active ? 1u : 0u,
                                k_msgq_num_used_get(&mesh_rx_msgq),
                                report_tx_queue_used(),
                                mesh_relay_tx_active(&mesh_runtime) ? 1u : 0u,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                mesh_ch9_tx_pending.active ? 1u : 0u,
                                app_node_comm_gateway_route_refresh_due_ms(),
                                mesh_channel9_connection_count());
        }
    }
    if (!mesh_role_uses_uwb_rx()) {
        mesh_uwb_rx_active = false;
        return;
    }
    mesh_coordinator_decide_now("uwb-rx", &coordinator_decision);
    if (!coordinator_decision.uwb_rx_allowed) {
        if (coordinator_decision.route_wait_allowed &&
            DEVICE_ROLE == ROLE_ANCHOR &&
            mesh_route_waiting_tx_valid) {
            mesh_try_route_waiting_tx();
        }
        if (coordinator_decision.report_tx_allowed) {
            report_tx_schedule(0u);
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_UWB_RX_COORD_DEFER state=%s reason=%s route=%u q=%u\n",
                                app_mesh_coordinator_state_name(
                                    coordinator_decision.state),
                                coordinator_decision.reason == NULL ? "none" :
                                coordinator_decision.reason,
                                mesh_route_waiting_tx_valid ? 1u : 0u,
                                report_tx_queue_used());
        }
        mesh_schedule_uwb_rx(MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
        return;
    }
    if ((DEVICE_ROLE == ROLE_ANCHOR &&
         (anchor_uwb_window_active() || mesh_report_anchor_survey_discovery_is_pending())) ||
        mesh_pending_tx_blocks_uwb_rx()) {
        mesh_schedule_uwb_rx(mesh_uwb_rx_idle_delay_ms());
        return;
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && mesh_route_waiting_tx_valid) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            LOG_INF("mesh UWB RX suppressed while route-waiting TX owns channel-9 event");
        }
        mesh_try_route_waiting_tx();
        mesh_schedule_uwb_rx(MESH_ROUTE_WAIT_RX_SUPPRESS_MS);
        return;
    }

    if (mesh_gateway_route_test_role()) {
        uint32_t now_ms = k_uptime_get_32();
        bool route_adv_pending;

        if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_YIELD_ADV now=%u due=%u\n",
                                k_uptime_get_32(),
                                app_node_comm_gateway_route_refresh_due_ms());
            (void)app_node_comm_gateway_route_refresh_schedule_now();
            mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
            return;
        }

        route_adv_pending = app_node_comm_gateway_route_refresh_pending_wait_ms(
            now_ms,
            &route_adv_wait_ms);
        window_ms = app_mesh_rx_policy_gateway_ch9_window_ms(
            UWB_MESH_GATEWAY_RX_WINDOW_MS,
            route_adv_pending,
            route_adv_wait_ms);
        if (route_adv_pending &&
            window_ms < UWB_MESH_GATEWAY_RX_WINDOW_MS) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_CAP now=%u win=%u adv_wait=%u due=%u\n",
                                k_uptime_get_32(),
                                window_ms,
                                route_adv_wait_ms,
                                app_node_comm_gateway_route_refresh_due_ms());
        }
        uint32_t gateway_rx_deadline_ms = k_uptime_get_32() + window_ms;

        ret = -ETIMEDOUT;
        while (!uptime_deadline_reached(k_uptime_get_32(), gateway_rx_deadline_ms)) {
            uint32_t remaining_ms =
                uptime_ms_until_deadline(k_uptime_get_32(), gateway_rx_deadline_ms);
            uint32_t ack_retry_wait_ms = 0u;

            if (mesh_gateway_handoff_due_batch_acks(k_uptime_get_32())) {
                /* Let the node-communication worker run the frozen ACK. */
                mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
                return;
            }
            if (mesh_gateway_batch_ack_retry_wait_ms(
                    k_uptime_get_32(), &ack_retry_wait_ms) &&
                ack_retry_wait_ms < remaining_ms) {
                remaining_ms = ack_retry_wait_ms;
            }

            if (remaining_ms == 0u) {
                continue;
            }
            if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
                break;
            }

            ret = mesh_rx_radio_start("mesh gateway continuous channel9 RX");
            if (ret < 0) {
                status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_GUARD_FAIL ret=%d\n", ret);
                mesh_schedule_uwb_rx(0u);
                return;
            }

            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            uwb_window_start_ms = k_uptime_get();
            status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_ARM now=%u rem=%u win=%u frames=%u\n",
                                k_uptime_get_32(),
                                remaining_ms,
                                window_ms,
                                channel9_frames_seen);
            ret = dwm3000_driver_configure_mesh_payload_mode();
            mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
            if (ret == 0) {
                ret = dwm3000_driver_receive_frame_continuous(remaining_ms,
                                                              frame,
                                                              UWB_MESH_MAX_FRAME_LEN,
                                                              &frame_len,
                                                              &quality,
                                                              NULL,
                                                              &rx_failure);
            }

            if (ret == 0) {
                observed_packet_ms = k_uptime_get_32();
                channel9_frames_seen++;
                status_debug_note("DBG_GATEWAY_CH9_RX_CONT_FRAME\n");
                status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
                (void)dwm3000_driver_idle();
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_GATEWAY_CH9_RX_RELEASE_IDLE\n");
                }
            } else if (ret == -ECANCELED) {
                (void)dwm3000_driver_idle();
                status_debug_printf("DBG_GATEWAY_CH9_RX_COMMAND_PREEMPT now=%u rem=%u frames=%u\n",
                                    k_uptime_get_32(),
                                    remaining_ms,
                                    channel9_frames_seen);
            } else if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
                           ret, rx_failure)) {
                if (gateway_ch9_recoverable_rx_errors < UINT16_MAX) {
                    gateway_ch9_recoverable_rx_errors++;
                }
                /* The driver already forces RX off and clears terminal status. */
                (void)dwm3000_driver_idle();
            } else {
                (void)dwm3000_driver_standby();
            }
            mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
            mesh_rx_radio_stop();
            app_watchdog_note_radio_progress();

            if (ret != 0) {
                if (ret == -ECANCELED) {
                    int priority_ret =
                        app_node_comm_gateway_delivery_safe_boundary();

                    if (priority_ret < 0) {
                        LOG_ERR("gateway command safe-boundary scheduling failed after RX abort: %d",
                                priority_ret);
                    }
                    break;
                }
                if (app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
                        ret, rx_failure)) {
                    if (gateway_ch9_recoverable_rx_errors <= 3u ||
                        (gateway_ch9_recoverable_rx_errors % 64u) == 0u) {
                        status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_RECOVER ret=%d fail=%u rem=%u errors=%u frames=%u\n",
                                            ret,
                                            (unsigned int)rx_failure,
                                            remaining_ms,
                                            gateway_ch9_recoverable_rx_errors,
                                            channel9_frames_seen);
                    }
                    continue;
                }
                if (ret != -ETIMEDOUT) {
                    status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_FAIL ret=%d fail=%u\n",
                                        ret,
                                        (unsigned int)rx_failure);
                    LOG_WRN("mesh gateway continuous channel-9 RX failed: ret=%d", ret);
                }
                break;
            }

            if (dwm3000_driver_receive_abort_pending()) {
                bool valid_mesh_frame = false;

                (void)mesh_queue_from_frame_deferred(frame,
                                                     frame_len,
                                                     quality,
                                                     UWB_CHANNEL_MESH_PAYLOAD,
                                                     &valid_mesh_frame,
                                                     NULL);
                status_debug_printf("DBG_GATEWAY_CH9_RX_FRAME_DEFER_COMMAND now=%u valid=%u frames=%u\n",
                                    k_uptime_get_32(),
                                    valid_mesh_frame ? 1u : 0u,
                                    channel9_frames_seen);
                {
                    int priority_ret =
                        app_node_comm_gateway_delivery_safe_boundary();

                    if (priority_ret < 0) {
                        LOG_ERR("gateway command safe-boundary scheduling failed after RX frame: %d",
                                priority_ret);
                    }
                }
                break;
            }

            {
                bool accepted = mesh_process_received_frame(frame,
                                                            frame_len,
                                                            quality,
                                                            true,
                                                            0u,
                                                            NULL,
                                                            observed_packet_ms,
                                                            NULL);
                bool processed = mesh_process_queued_rx_now(
                    "gateway-ch9-continuous-rx");

                status_debug_printf("DBG_GATEWAY_CH9_FRAME_PROCESS len=%u quality=%u accepted=%u processed=%u frames=%u\n",
                                    (unsigned int)frame_len,
                                    quality,
                                    accepted ? 1u : 0u,
                                    processed ? 1u : 0u,
                                    channel9_frames_seen);
            }
        }

        status_debug_printf("DBG_GATEWAY_CH9_RX_CONT_DONE ret=%d len=%u fail=%u frames=%u recover=%u\n",
                            ret,
                            (unsigned int)frame_len,
                            (unsigned int)rx_failure,
                            channel9_frames_seen,
                            gateway_ch9_recoverable_rx_errors);
        if (app_node_comm_gateway_route_refresh_due(k_uptime_get_32())) {
            status_debug_printf("DBG_GATEWAY_CH9_RX_DONE_YIELD_ADV now=%u due=%u\n",
                                k_uptime_get_32(),
                                app_node_comm_gateway_route_refresh_due_ms());
            (void)app_node_comm_gateway_route_refresh_schedule_now();
            mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
            return;
        }
        mesh_schedule_uwb_rx(0u);
        return;
    }

    gateway_route_preempt = mesh_gateway_route_test_preempt_active(k_uptime_get_32());
    gateway_continuous_ch5 = mesh_gateway_route_test_role() &&
                             mesh_channel9_connection_count() == 0u;
    if (app_mesh_c5_gateway_rx_should_yield_to_response(
            &(const struct app_mesh_c5_flood_priority_state) {
                .response_priority =
                    app_node_comm_gateway_route_refresh_response_priority_due(
                        k_uptime_get_32()),
                .gateway_ch5_preempt = gateway_route_preempt,
            })) {
        (void)app_node_comm_gateway_route_refresh_schedule_now();
        mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
        return;
    }
    channel9_event = !gateway_route_preempt &&
                     mesh_select_channel9_ack_tx_event(k_uptime_get_32(),
                                                       &channel9_plan,
                                                       &channel9_peer_id);
    if (channel9_event) {
        (void)mesh_send_pending_ch9_ack_batch(&channel9_plan,
                                              channel9_peer_id,
                                              "ch9-ack-batch-slot");
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    channel9_event = !gateway_route_preempt &&
                     mesh_select_channel9_rx_event(k_uptime_get_32(),
                                                   &channel9_plan,
                                                   &channel9_peer_id,
                                                   &channel9_timing_index);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
        !channel9_event) {
        uint32_t next_delay_ms =
            mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

        status_debug_printf("DBG_TX_CH9_RX_WAIT next_delay=%u conn=%u ack=%u\n",
                            next_delay_ms,
                            mesh_channel9_connection_count(),
                            mesh_ch9_tx_pending.active ? 1u : 0u);
        mesh_schedule_uwb_rx(next_delay_ms);
        return;
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !channel9_event &&
        mesh_channel9_connection_count() == 0u) {
        mesh_anchor_yield_idle_ch5_to_low_duty_scan("anchor-low-duty-ch5-owner");
        return;
    }
    if (gateway_route_preempt && !channel9_event) {
        status_debug_printf("DBG_GATEWAY_CH5_PREEMPT_SCAN peer=0x%llx now=%u until=%u\n",
                            (unsigned long long)mesh_gateway_route_preempt_peer_id,
                            k_uptime_get_32(),
                            mesh_gateway_route_preempt_deadline_ms);
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_channel9_connection_count() > 0u &&
        !channel9_event) {
        channel5_gap_window_ms = mesh_active_channel9_ch5_gap_window_ms(k_uptime_get_32());
        if (channel5_gap_window_ms == 0u) {
            uint32_t next_delay_ms = mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                mesh_channel9_connection_count() > 0u &&
                next_delay_ms > MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS) {
                status_debug_printf("DBG_CH5_GAP_SCAN_SKIP now=%u next_delay=%u\n",
                                    k_uptime_get_32(),
                                    next_delay_ms);
            }
            next_delay_ms =
                app_mesh_c5_connected_gap_reschedule_ms(
                    next_delay_ms,
                    MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
                    MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS);
            mesh_schedule_uwb_rx(next_delay_ms);
            return;
        }
        channel5_gap_scan = true;
    }

    ret = mesh_rx_radio_start(channel9_event ?
                              "mesh channel9 UWB RX" :
                              "mesh UWB RX");
    if (ret < 0) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
            status_debug_printf("DBG_RX_GUARD_FAIL mode=%u ret=%d\n",
                                channel9_event ? 9u : 5u,
                                ret);
            LOG_WRN("mesh gateway RX window guard failed: mode=%s ret=%d",
                    channel9_event ? "ch9" : "ch5",
                    ret);
        }
        mesh_schedule_uwb_rx(mesh_gateway_route_test_preempt_active(k_uptime_get_32()) ?
                             0u : mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    window_ms = channel9_event ? channel9_plan.window_ms :
                (gateway_route_preempt && !channel5_gap_scan) ?
                    mesh_gateway_route_test_preempt_window_ms(k_uptime_get_32()) :
                channel5_gap_scan ? channel5_gap_window_ms :
                gateway_continuous_ch5 ? MESH_GATEWAY_CH5_CONTINUOUS_RX_MS :
                mesh_uwb_rx_window_ms();
    if (gateway_route_preempt && !channel5_gap_scan) {
        uint32_t response_wait_ms =
            app_node_comm_gateway_route_refresh_response_priority_wait_ms(
                k_uptime_get_32());

        if (response_wait_ms > 0u && response_wait_ms < window_ms) {
            window_ms = response_wait_ms;
        }
    }
    if (channel5_gap_scan) {
        channel5_gap_deadline_ms = k_uptime_get_32() + window_ms;
    }
    uwb_window_start_ms = k_uptime_get();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
        mesh_ch9_event_set(CH9_EVENT_GRANTED,
                           channel9_peer_id,
                           &channel9_plan,
                           "rx-slot");
        status_debug_printf("DBG_CH9_RX_START idx=%u now=%u start=%u end=%u tail=%u\n",
                            channel9_timing_index,
                            k_uptime_get_32(),
                            channel9_plan.start_ms,
                            channel9_plan.end_ms,
                            MESH_EVENT_RX_LATE_GUARD_MS);
    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
               (channel5_gap_scan || gateway_route_preempt ||
                gateway_continuous_ch5)) {
        status_debug_printf("DBG_CH5_GAP_SCAN now=%u win=%u next_delay=%u\n",
                            k_uptime_get_32(),
                            window_ms,
                            mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    }
    ret = channel9_event ?
          dwm3000_driver_configure_mesh_payload_mode() :
          dwm3000_driver_configure_wake_mode();
    if (channel9_event) {
        mesh_event_note_channel_switch(&mesh_event_stats, ret == 0, false);
    }
    if (ret == 0 && channel9_event) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_CH9_RX_CONFIG_OK\n");
            status_debug_printf("DBG_CH9_RX_EARLY_ARM now=%u start=%u guard=%u\n",
                                k_uptime_get_32(),
                                channel9_plan.start_ms,
                                MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS);
        } else {
            mesh_wait_until_ms(channel9_plan.start_ms);
        }
    } else if (channel9_event && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_RX_CONFIG_FAIL\n");
        status_debug_printf("DBG_CH9_RX_CONFIG_FAIL ret=%d now=%u start=%u\n",
                            ret,
                            k_uptime_get_32(),
                            channel9_plan.start_ms);
    }
    if (ret == 0 && channel9_event && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        const uint32_t deadline_ms = channel9_plan.end_ms + MESH_EVENT_RX_LATE_GUARD_MS;

        frame_processed_inline = true;
        ret = -ETIMEDOUT;
        channel9_rx_armed_ms = k_uptime_get_32();
        status_debug_printf("DBG_CH9_RX_ARMED now=%u start=%u end=%u deadline=%u\n",
                            channel9_rx_armed_ms,
                            channel9_plan.start_ms,
                            channel9_plan.end_ms,
                            deadline_ms);
        while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms)) {
            struct dwm3000_rx_frame_timing rx_timing = {0};
            bool peer_observed_this_frame = false;
            uint32_t remaining_ms = uptime_ms_until_deadline(k_uptime_get_32(), deadline_ms);

            if (remaining_ms == 0u) {
                break;
            }
            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            ret = dwm3000_driver_receive_frame_continuous_timed(remaining_ms,
                                                                frame,
                                                                UWB_MESH_MAX_FRAME_LEN,
                                                                &frame_len,
                                                                &quality,
                                                                NULL,
                                                                &rx_failure,
                                                                &rx_timing);
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret != 0) {
                break;
            }

            channel9_frames_seen++;
            status_debug_note("DBG_CH9_RX_FRAME\n");
            status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_MESH_PAYLOAD);
            observed_packet_ms = k_uptime_get_32();
            if (rx_timing.valid) {
                int32_t host_delta_ms = (int32_t)(observed_packet_ms - channel9_plan.start_ms);
                int32_t arm_delta_ms = (int32_t)(channel9_rx_armed_ms - channel9_plan.start_ms);
                int32_t hw_slot_delta_us =
                    (arm_delta_ms * 1000) + (int32_t)rx_timing.rx_since_enable_uus;

                status_debug_printf("DBG_CH9_RX_HW since_us=%u host_delta=%d en32=0x%08x rx32=0x%08x\n",
                                    rx_timing.rx_since_enable_uus,
                                    host_delta_ms,
                                    rx_timing.rx_enable_time32,
                                    rx_timing.rx_timestamp_time32);
                status_debug_printf("DBG_CH9_RX_TIMING arm_delta=%d hw_delta_us=%d host_delta=%d len=%u q=%u\n",
                                    arm_delta_ms,
                                    hw_slot_delta_us,
                                    host_delta_ms,
                                    (unsigned int)frame_len,
                                    quality);
            }
            (void)mesh_process_received_frame(frame,
                                              frame_len,
                                              quality,
                                              true,
                                              channel9_peer_id,
                                              &channel9_plan,
                                              observed_packet_ms,
                                              &peer_observed_this_frame);
            channel9_peer_observed = channel9_peer_observed || peer_observed_this_frame;
        }
    } else if (ret == 0 && channel9_event) {
        ret = dwm3000_driver_receive_frame(window_ms,
                                           frame,
                                           UWB_MESH_MAX_FRAME_LEN,
                                           &frame_len,
                                           &quality,
                                           NULL);
    } else if (ret == 0 && channel5_gap_scan) {
        frame_processed_inline = true;
        ret = -ETIMEDOUT;
        while (!uptime_deadline_reached(k_uptime_get_32(),
                                        channel5_gap_deadline_ms)) {
            struct uwb_wake_claim_frame claim;
            enum app_mesh_c5_connected_gap_rx_action action;
            uint32_t remaining_ms = uptime_ms_until_deadline(
                k_uptime_get_32(), channel5_gap_deadline_ms);
            bool click_claim;

            if (remaining_ms == 0u) {
                break;
            }
            frame_len = 0u;
            quality = 0u;
            rx_failure = DWM3000_RX_FAILURE_NONE;
            ret = dwm3000_driver_receive_frame_continuous(
                remaining_ms,
                frame,
                UWB_MESH_MAX_FRAME_LEN,
                &frame_len,
                &quality,
                NULL,
                &rx_failure);
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
                channel5_gap_last_error = ret;
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_printf("DBG_CH5_GAP_REARM_ERROR ret=%d fail=%u rem=%u\n",
                                        ret,
                                        (unsigned int)rx_failure,
                                        remaining_ms);
                }
                continue;
            }

            observed_packet_ms = k_uptime_get_32();
            status_debug_gateway_uwb_rx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
            click_claim = mesh_frame_requires_anchor_click_handoff(frame,
                                                                   frame_len,
                                                                   &claim);
            action = app_mesh_c5_connected_gap_rx_action(
                click_claim,
                uptime_deadline_reached(k_uptime_get_32(),
                                        channel5_gap_deadline_ms));
            if (action == APP_MESH_C5_CONNECTED_GAP_RX_HANDOFF_CLICK) {
                frame_processed_inline = false;
                ret = 0;
                break;
            }

            channel5_gap_frames_seen++;
            (void)mesh_process_received_frame(frame,
                                              frame_len,
                                              quality,
                                              false,
                                              0u,
                                              NULL,
                                              observed_packet_ms,
                                              NULL);
            if (action == APP_MESH_C5_CONNECTED_GAP_RX_COMPLETE) {
                break;
            }
        }
        if (frame_processed_inline) {
            ret = -ETIMEDOUT;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_printf("DBG_CH5_GAP_SCAN_DONE frames=%u last_error=%d now=%u deadline=%u click=%u\n",
                                channel5_gap_frames_seen,
                                channel5_gap_last_error,
                                k_uptime_get_32(),
                                channel5_gap_deadline_ms,
                                frame_processed_inline ? 0u : 1u);
        }
    } else if (ret == 0) {
        ret = dwm3000_driver_receive_frame_continuous(window_ms,
                                                      frame,
                                                      UWB_MESH_MAX_FRAME_LEN,
                                                      &frame_len,
                                                      &quality,
                                                      NULL,
                                                      &rx_failure);
    }
    if (ret == 0 && !frame_processed_inline) {
        status_debug_gateway_uwb_rx_channel_pulse(channel9_event ? UWB_CHANNEL_MESH_PAYLOAD :
                                                                  UWB_CHANNEL_WAKE_CONTACT);
        observed_packet_ms = k_uptime_get_32();
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        DEVICE_ROLE == ROLE_GATEWAY && !channel9_event) {
        (void)dwm3000_driver_idle();
    } else {
        mesh_release_radio_after_mesh_turn(channel9_event,
                                           channel9_event ? "ch9-rx" : "mesh-rx");
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    mesh_rx_radio_stop();

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && DEVICE_ROLE == ROLE_GATEWAY) {
        uint32_t now_ms = k_uptime_get_32();
        bool log_window = ret == 0 || ret != -ETIMEDOUT || channel9_event ||
                          mesh_rx_window_log_next_ms == 0u ||
                          now_ms >= mesh_rx_window_log_next_ms;

        if (log_window) {
            mesh_rx_window_log_next_ms = now_ms + MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS;
            if (channel9_event || ret != -ETIMEDOUT) {
                status_debug_printf("DBG_RX_DONE mode=%u ret=%d len=%u fail=%u frames=%u\n",
                                    channel9_event ? 9u : 5u,
                                    ret,
                                    (unsigned int)frame_len,
                                    (unsigned int)rx_failure,
                                    channel9_frames_seen);
            }
        }
    }

    if (ret == 0 && !frame_processed_inline) {
        (void)mesh_process_received_frame(frame,
                                          frame_len,
                                          quality,
                                          channel9_event,
                                          channel9_peer_id,
                                          &channel9_plan,
                                          observed_packet_ms,
                                          &channel9_peer_observed);
    } else if (ret != -ETIMEDOUT) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
            status_debug_note("DBG_CH9_RX_FAIL\n");
        }
        LOG_WRN("mesh UWB RX failed: ret=%d role=%s", ret, role_name());
	    } else if (channel9_event && !channel9_peer_observed &&
	               channel9_timing_index < MESH_RELAY_EVENT_TIMINGS &&
	               mesh_runtime.event_timings[channel9_timing_index].valid) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note(channel9_frames_seen == 0u ?
	                              "DBG_CH9_RX_EMPTY\n" :
	                              "DBG_CH9_RX_NO_EXPECTED_PEER\n");
	            status_debug_note("DBG_CH9_RX_TIMEOUT\n");
	            status_debug_printf("DBG_CH9_RX_MISS frames=%u now=%u start=%u end=%u fail=%u\n",
	                                channel9_frames_seen,
	                                k_uptime_get_32(),
	                                channel9_plan.start_ms,
	                                channel9_plan.end_ms,
	                                (unsigned int)rx_failure);
	        }
	        mesh_relay_note_channel9_missed(&mesh_runtime,
	                                        channel9_peer_id,
	                                        &mesh_event_stats);
	        app_mesh_test_note_ch9_missed();
		    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
		        status_debug_note(channel9_peer_observed ?
		                          "DBG_CH9_RX_DONE_WITH_PEER\n" :
		                          "DBG_CH9_RX_TIMEOUT\n");
		    }

    if (channel9_event) {
        if (ret != 0 && ret != -ETIMEDOUT) {
            mesh_ch9_event_set(CH9_EVENT_BUSY_RETRY_LATER,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot-fail");
        } else if (channel9_peer_observed) {
            mesh_ch9_event_set(CH9_EVENT_COMPLETE,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot");
        } else {
            mesh_ch9_event_set(CH9_EVENT_WINDOW_EXPIRED,
                               channel9_peer_id,
                               &channel9_plan,
                               "rx-slot");
        }
    }

    if (channel5_gap_scan) {
        uint32_t next_delay_ms = mesh_next_channel9_rx_delay_ms(k_uptime_get_32());

        mesh_schedule_uwb_rx(
            app_mesh_c5_connected_gap_reschedule_ms(
                next_delay_ms,
                MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS,
                MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS));
    } else if (app_mesh_c5_gateway_rx_should_yield_to_response(
            &(const struct app_mesh_c5_flood_priority_state) {
                .response_priority =
                    app_node_comm_gateway_route_refresh_response_priority_due(
                        k_uptime_get_32()),
                .gateway_ch5_preempt =
                    mesh_gateway_route_test_preempt_active(k_uptime_get_32()),
            })) {
        (void)app_node_comm_gateway_route_refresh_schedule_now();
        mesh_schedule_uwb_rx(MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
    } else if (mesh_gateway_route_test_preempt_active(k_uptime_get_32()) ||
               (mesh_gateway_route_test_role() &&
                mesh_channel9_connection_count() == 0u)) {
        mesh_schedule_uwb_rx(0u);
    } else {
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    }
}

int mesh_start_uwb_rx(const char *reason)
{
    int ret;

    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }

    ret = mesh_schedule_uwb_rx(0u);
    if (ret < 0) {
        LOG_ERR("mesh UWB RX schedule failed: role=%s reason=%s ret=%d",
                role_name(),
                reason == NULL ? "start" : reason,
                ret);
        return ret;
    }
    if (mesh_gateway_route_test_role()) {
        status_debug_printf("DBG_GATEWAY_RX_START reason=%s active=%u qrx=%u qtx=%u adv_due=%u\n",
                            reason == NULL ? "start" : reason,
                            mesh_uwb_rx_active ? 1u : 0u,
                            k_msgq_num_used_get(&mesh_rx_msgq),
                            report_tx_queue_used(),
                            app_node_comm_gateway_route_refresh_due_ms());
    }
    LOG_INF("mesh UWB RX scheduled: role=%s window_ms=%u idle_ms=%u reason=%s",
            role_name(),
            mesh_uwb_rx_window_ms(),
            mesh_uwb_rx_idle_delay_ms(),
            reason == NULL ? "start" : reason);
    return 0;
}


static bool mesh_route_refresh_allowed(void *ctx)
{
    ARG_UNUSED(ctx);
    return app_mesh_c5_gateway_route_adv_allowed(
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST));
}

static bool mesh_route_refresh_policy_running(void *ctx)
{
    ARG_UNUSED(ctx);
    return app_node_comm_policy_running();
}

static bool mesh_route_refresh_response_active(uint32_t now_ms, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!mesh_gateway_route_test_role() ||
        mesh_gateway_route_preempt_peer_id == 0u ||
        !mesh_gateway_route_test_preempt_active(now_ms)) {
        return false;
    }
    return mesh_c5_contact_active(mesh_gateway_route_preempt_peer_id,
                                  C5_CONTACT_PURPOSE_ROUTE_SOLICIT,
                                  now_ms);
}

static uint32_t mesh_route_refresh_now(void *ctx)
{
    ARG_UNUSED(ctx);
    return k_uptime_get_32();
}

static int mesh_route_refresh_build(void *ctx,
                                    uint32_t sequence,
                                    uint32_t now_ms,
                                    struct mesh_gateway_route_adv_snapshot *snapshot,
                                    struct mesh_outbound *out)
{
    int ret;

    ARG_UNUSED(ctx);
    if (snapshot == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!snapshot->valid) {
        ret = mesh_relay_capture_gateway_route_adv_snapshot(
            &mesh_runtime, sequence, now_ms, snapshot);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    return mesh_relay_build_gateway_route_adv_from_snapshot(&mesh_runtime,
                                                            snapshot,
                                                            out);
}

static int mesh_route_refresh_send_wake(void *ctx, const char *reason)
{
    ARG_UNUSED(ctx);
    return mesh_send_route_wake_train(
        MESH_BROADCAST_ID, NULL, NULL,
        C5_CONTACT_PURPOSE_ROUTE_CONTACT_REFRESH, reason);
}

static void mesh_route_refresh_note_sent(const struct mesh_outbound *out,
                                         uint32_t now_ms,
                                         void *ctx)
{
    ARG_UNUSED(ctx);
    mesh_relay_note_tx_sent(&mesh_runtime, out, now_ms);
    high_debug_log_event("GATEWAY_ROUTE_ADV",
                         "phase=sent seq=%u ttl=%u",
                         out->packet.session_id,
                         out->packet.ttl);
}

static void mesh_route_refresh_stop_scan(void *ctx)
{
    ARG_UNUSED(ctx);
    if (mesh_gateway_route_test_role()) {
        mesh_stop_role_scan();
    }
}

static void mesh_route_refresh_restart_scan(void *ctx)
{
    ARG_UNUSED(ctx);
    if (mesh_gateway_route_test_role()) {
        mesh_restart_role_scan();
    }
}

static void mesh_route_refresh_clear_response(void *ctx)
{
    ARG_UNUSED(ctx);
    mesh_gateway_route_test_clear_preempt(0u, "route-adv-sent");
}

static int mesh_route_refresh_schedule(void *ctx,
                                       struct k_work_delayable *work,
                                       uint32_t delay_ms)
{
    ARG_UNUSED(ctx);
    return mesh_reschedule_delayable(work, delay_ms);
}

static bool mesh_route_refresh_defer(void *ctx)
{
    struct mesh_c5_flood_tx_context flood_ctx = {
        .response_priority =
            app_node_comm_gateway_route_refresh_response_priority_due(
                k_uptime_get_32()),
    };

    ARG_UNUSED(ctx);
    return mesh_c5_flood_defer_active_cb(&flood_ctx);
}

static void mesh_route_refresh_observe(
    void *ctx, const struct app_node_comm_route_refresh_event *event)
{
    ARG_UNUSED(ctx);
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_route_refresh_event != NULL) {
        mesh_report_callbacks->gateway_route_refresh_event(event);
    }
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    struct app_node_comm_gateway_control_config gateway_control_config = {
        .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
        .send_flood = mesh_gateway_control_send_flood,
        .priority_observer = mesh_gateway_control_priority_observed,
    };
    const struct app_mesh_report_encode_ops report_encode_config = {
        .queue_cir_fragment = mesh_queue_anchor_cir_fragment,
    };
    static const struct app_node_comm_gateway_route_refresh_config
        route_refresh_config = {
            .gateway_role = DEVICE_ROLE == ROLE_GATEWAY,
            .wake_train_ms = WAKE_ADV_MS,
            .allowed = mesh_route_refresh_allowed,
            .policy_running = mesh_route_refresh_policy_running,
            .response_priority_active = mesh_route_refresh_response_active,
            .now_ms = mesh_route_refresh_now,
            .random_u32 = mesh_c5_flood_random_u32,
            .sleep_until_ms = mesh_c5_flood_sleep_until_ms,
            .defer_active = mesh_route_refresh_defer,
            .c5_quiet = mesh_c5_flood_quiet_cb,
            .send = mesh_c5_flood_send_cb,
            .build = mesh_route_refresh_build,
            .send_wake = mesh_route_refresh_send_wake,
            .note_sent = mesh_route_refresh_note_sent,
            .stop_role_scan = mesh_route_refresh_stop_scan,
            .restart_role_scan = mesh_route_refresh_restart_scan,
            .clear_response_priority = mesh_route_refresh_clear_response,
            .schedule = mesh_route_refresh_schedule,
            .observe = mesh_route_refresh_observe,
        };

    mesh_report_callbacks = callbacks;
    app_mesh_report_encode_init(&report_encode_config);
    atomic_set(&mesh_transport_paused_state, 0);
    app_mesh_rx_handoff_reset(&mesh_rx_handoff);
    app_mesh_paused_delivery_reset(&mesh_paused_delivery);
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    k_work_queue_start(&mesh_route_work_q,
                       mesh_route_work_q_stack,
                       K_THREAD_STACK_SIZEOF(mesh_route_work_q_stack),
                       MESH_ROUTE_WORKQUEUE_PRIORITY,
                       &mesh_route_work_q_config);
    gateway_control_config.priority_work_queue = &mesh_route_work_q;
#endif
    app_node_comm_gateway_control_init(&gateway_control_config);
    app_node_comm_gateway_route_refresh_init(&route_refresh_config,
                                             (uint32_t)DEVICE_ID);
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init(&mesh_uwb_rx_rearm_work, mesh_uwb_rx_rearm_work_handler);
    k_work_init_delayable(&mesh_persistence_retry_work,
                          mesh_persistence_retry_work_handler);
    mesh_outbox_persistence_dirty = false;
    mesh_child_custody_persistence_dirty = false;
    mesh_persistence_retry_round = 0u;
    memset(&mesh_delivery_health, 0, sizeof(mesh_delivery_health));
    app_mesh_ch9_ack_table_init(&mesh_ch9_ack_table);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
#endif
    k_work_init_delayable(&mesh_c5_flood_work, mesh_c5_flood_work_handler);
    k_work_init_delayable(&mesh_route_discovery_work, mesh_route_discovery_work_handler);
    k_work_init_delayable(&mesh_event_negotiation_retry_work,
                          mesh_event_negotiation_retry_work_handler);
    mesh_event_propose_clear();
    memset(&mesh_event_accept_retry, 0, sizeof(mesh_event_accept_retry));
    app_mesh_event_retry_clear(&mesh_event_accept_rx_cache);
#if DEVICE_ROLE == ROLE_ANCHOR
    k_work_init_delayable(&mesh_route_request_action_work,
                          mesh_route_request_action_work_handler);
    mesh_route_request_action_pending = false;
#endif
    return 0;
}

void mesh_delivery_health_get(struct mesh_delivery_health *health)
{
    if (health != NULL) {
        *health = mesh_delivery_health;
    }
}

int mesh_gateway_command_priority_submit(struct k_work_delayable *work)
{
    return app_node_comm_gateway_control_priority_submit(work);
}

int mesh_gateway_command_priority_safe_boundary(void)
{
    return app_mesh_command_orchestrator_safe_boundary(
        app_node_comm_gateway_control_context(),
        radio_guard_uwb_busy(),
        app_node_comm_gateway_control_safe_boundary_schedule,
        NULL);
}

static void mesh_gateway_control_priority_observed(void *ctx, int result)
{
    ARG_UNUSED(ctx);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_GATEWAY_COMMAND_PRIORITY_SUBMIT ret=%d now=%u\n",
                            result,
                            k_uptime_get_32());
    }
}
