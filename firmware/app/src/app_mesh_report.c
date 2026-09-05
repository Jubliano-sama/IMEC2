#include "app_mesh_report.h"
#include "app_mesh_report_encode.h"
#include "app_anchor.h"
#include "app_anchor_click_event_runtime.h"

#include "app_board.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_durable_state.h"
#include "app_gateway_assignment_publisher.h"
#include "app_gateway_control_sequence.h"
#include "app_gateway_ble.h"
#include "app_mesh_c5_priority.h"
#include "app_mesh_direct_probe_diag.h"
#include "app_mesh_direct_gateway_retry.h"
#include "app_mesh_event_retry.h"
#include "app_mesh_command_orchestrator.h"
#include "app_mesh_flood.h"
#include "app_mesh_ch9_ack.h"
#include "app_mesh_collection_deferral.h"
#include "app_mesh_gateway_ack_policy.h"
#include "app_mesh_preemption.h"
#include "app_mesh_route_reply_ack.h"
#include "app_mesh_route_ready_handoff.h"
#include "app_mesh_async_route_request.h"
#include "app_mesh_route_request_policy.h"
#include "app_mesh_route_wait_tx.h"
#include "app_mesh_rf_retry.h"
#include "app_mesh_result_handoff.h"
#include "app_mesh_rx_policy.h"
#include "app_mesh_scheduler_liveness.h"
#include "app_mesh_test.h"
#include "app_mesh_tx_handoff_gate.h"
#include "app_node_comm.h"
#include "app_node_comm_gateway_control.h"
#include "app_node_comm_gateway_route_refresh.h"
#include "app_operation_policy.h"
#include "app_state.h"
#include "app_survey.h"
#include "app_stack_workload_diag.h"
#include "firmware_delivery_loss.h"
#include "firmware_state_machines.h"
#include "app_wake_train_politeness.h"
#include "app_watchdog.h"
#include "dwm3000_driver.h"
#include "dwm3000_timing.h"
#include "discovery_assignment.h"
#include "mesh.h"
#include "mesh_event_owner.h"
#include "mesh_event_owner_registry.h"
#include "mesh_packet_age.h"
#include "mesh_preemption.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "protocol_rx_lifecycle.h"
#include "report.h"
#include "route.h"
#include "semantic_digest.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_report, LOG_LEVEL_DBG);

/* RAM-only, per-anchor flood activation for the current enumeration epoch. */
static struct protocol_rx_downstream_activation
    mesh_enumeration_downstream_activation;
static bool mesh_enumeration_downstream_survey_follows;

/*
 * Post-burst click delivery listen window (Channel 5 Delivery Protocol §5).
 *
 * The anchor scan thread publishes the cohort and the window as soon as the
 * range schedule has been served; the mesh-route thread and the report worker
 * read it to decide whether a next hop is known to be listening.  Everything
 * here is RAM-only and survives exactly one burst.
 */
#if DEVICE_ROLE == ROLE_ANCHOR && \
    IS_ENABLED(CONFIG_IMEC_CLICK_DELIVERY_PARENT_LISTEN)
#define APP_MESH_CLICK_LISTEN_SUPPORTED 1
#else
#define APP_MESH_CLICK_LISTEN_SUPPORTED 0
#endif

#if APP_MESH_CLICK_LISTEN_SUPPORTED
struct app_click_listen_window {
    /* A FLAG_COUNT_AS_CLICK schedule is capped at this cohort size by
     * build_uwb_schedule_report_if_relevant(); static RAM on the anchor is
     * too tight to reserve the larger diagnostic schedule bound. */
    uint64_t participants[RANGE_REPORT_MAX_PARTICIPANT_ANCHORS];
    uint32_t start_ms;
    uint32_t until_ms;
    uint8_t count;
    bool valid;
};

static struct app_click_listen_window anchor_click_listen_window;
static struct k_spinlock anchor_click_listen_lock;
#endif

void app_mesh_report_note_click_listen_window(uint32_t window_start_ms,
                                              const uint64_t *participant_ids,
                                              uint8_t participant_count)
{
#if APP_MESH_CLICK_LISTEN_SUPPORTED
    struct app_click_listen_window published = {0};
    k_spinlock_key_t key;

    if (participant_ids == NULL || participant_count == 0u ||
        participant_count > RANGE_REPORT_MAX_PARTICIPANT_ANCHORS) {
        return;
    }
    for (uint8_t i = 0u; i < participant_count; i++) {
        published.participants[i] = participant_ids[i];
    }
    published.count = participant_count;
    published.start_ms = window_start_ms;
    published.until_ms = window_start_ms +
                         MESH_CLICK_DELIVERY_LISTEN_MS(participant_count);
    published.valid = true;

    key = k_spin_lock(&anchor_click_listen_lock);
    anchor_click_listen_window = published;
    k_spin_unlock(&anchor_click_listen_lock, key);

    status_debug_printf("DBG_CLICK_LISTEN start=%u until=%u n=%u\n",
                        published.start_ms,
                        published.until_ms,
                        (unsigned int)participant_count);
#else
    (void)window_start_ms;
    (void)participant_ids;
    (void)participant_count;
#endif
}

bool app_mesh_report_click_listen_active(uint32_t now_ms,
                                         uint32_t *remaining_ms)
{
#if APP_MESH_CLICK_LISTEN_SUPPORTED
    struct app_click_listen_window window;
    k_spinlock_key_t key;

    key = k_spin_lock(&anchor_click_listen_lock);
    window = anchor_click_listen_window;
    k_spin_unlock(&anchor_click_listen_lock, key);
    if (!window.valid) {
        return false;
    }
    /*
     * The window opens with the shared delivery grid.  Before that the anchor
     * is still ranging or building its report and must not hold the radio.
     */
    if ((int32_t)(now_ms - window.start_ms) < 0 ||
        (int32_t)(now_ms - window.until_ms) >= 0) {
        return false;
    }
    if (remaining_ms != NULL) {
        *remaining_ms = window.until_ms - now_ms;
    }
    return true;
#else
    (void)now_ms;
    (void)remaining_ms;
    return false;
#endif
}

bool app_mesh_report_click_participant(uint64_t node_id, uint32_t now_ms)
{
#if APP_MESH_CLICK_LISTEN_SUPPORTED
    struct app_click_listen_window window;
    k_spinlock_key_t key;

    if (node_id == 0u) {
        return false;
    }
    key = k_spin_lock(&anchor_click_listen_lock);
    window = anchor_click_listen_window;
    k_spin_unlock(&anchor_click_listen_lock, key);
    if (!window.valid || (int32_t)(now_ms - window.until_ms) >= 0) {
        return false;
    }
    for (uint8_t i = 0u; i < window.count; i++) {
        if (window.participants[i] == node_id) {
            return true;
        }
    }
    return false;
#else
    (void)node_id;
    (void)now_ms;
    return false;
#endif
}

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
#define MESH_ROUTE_REPLY_READY_POLL_MS 25u
#define MESH_EVENT_CONTROL_COMPACT_PAYLOAD_MAX 64u
#define MESH_EVENT_PROPOSE_OWNER_LIFETIME_MS 6000u
#define MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS 25000u
/* One ACCEPT response is expected 80 ms after PROPOSE.  A 250 ms receive turn
 * covers that response plus radio/worker margin without making the requester
 * inherit a multi-second response retry owned by the parent. */
#define MESH_EVENT_ACCEPT_REPLY_WAIT_MS 250u
/* A healthy one-report child link can send in its first event and close in its
 * next event. Retry after that expected release point, plus bounded per-node
 * jitter, so shared-parent contenders spread out without polling the relay. */
#define MESH_EVENT_PARENT_RELEASE_EXPECTED_MS \
    (MESH_EVENT_DEFAULT_FIRST_DELAY_MS + MESH_EVENT_DEFAULT_INTERVAL_MS + \
     MESH_EVENT_DEFAULT_GUARD_MS)
#define MESH_EVENT_PARENT_RETRY_JITTER_MS \
    (MESH_EVENT_DEFAULT_INTERVAL_MS / 4u)
#define MESH_EVENT_PARENT_RETRY_AFTER_TIMEOUT_MIN_MS \
    (MESH_EVENT_PARENT_RELEASE_EXPECTED_MS - MESH_EVENT_ACCEPT_REPLY_WAIT_MS)
/* Initial attempt plus three retained-route retries serves four children that
 * contend for one relay before an alternate route or discovery is used. */
#define MESH_KNOWN_PARENT_CONTACT_RETRIES 3u
#define MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS 1000u
#define MESH_EVENT_ACCEPT_REPLAY_LIFETIME_MS 6000u
#define MESH_EVENT_NEGOTIATION_DEADLINE_MS MESH_EVENT_ACCEPT_REPLY_WAIT_MS
_Static_assert(MESH_TOPOLOGY_PARENT_CONTACT_DEADLINE_MS <
                   DISCOVERY_ASSIGNMENT_RESPONSE_DIRECT_CUSTODY_MS,
               "topology contact deadline must fit direct response custody");
_Static_assert(MESH_EVENT_ACCEPT_REPLY_WAIT_MS >
                   MESH_RADIO_EVENT_ACCEPT_DELAY_MS +
                   MESH_RADIO_EVENT_ACCEPT_REALIGN_SLOP_MS,
               "ACCEPT reply window must cover response delay and slop");
_Static_assert(MESH_EVENT_PARENT_RELEASE_EXPECTED_MS >
                   MESH_EVENT_ACCEPT_REPLY_WAIT_MS,
               "parent retry must follow the ACCEPT receive turn");
_Static_assert(MESH_RADIO_ENUMERATION_ACTIVATION_WAKE_TRAIN_MS <=
                   UWB_WAKE_CLAIM_MAX_WAKE_TRAIN_MS,
               "enumeration activation wake train must fit the wake wire");
#define MESH_EVENT_ORIGIN_REPLAY_LIFETIME_MS \
    MESH_EVENT_ACCEPT_REPLAY_LIFETIME_MS
#define MESH_ROUTE_TEST_EMBEDDED_REPLY_GUARD_MS 5u
#define MESH_ROUTE_TEST_ROUTE_ADV_REPLY_GUARD_MS 20u
#define MESH_ROUTE_TEST_EMBEDDED_ROUTE_SUPPRESS_MS 1000u
#define MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS 50u
#define MESH_ROUTE_WAIT_RX_SUPPRESS_MS 100u
#define MESH_ROUTE_TEST_ROUTE_REPLY_RX_DELAY_MS \
    MESH_ROUTE_TEST_ROUTE_REPLY_RX_GUARD_MS
#define MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS 1000u
#if defined(CONFIG_IMEC_ML_ANCHOR)
#define MESH_CH9_TX_BATCH_MAX 1u
#elif defined(CONFIG_IMEC_MESH_ROUTE_TEST)
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
BUILD_ASSERT(MESH_ROUTE_REPLY_READY_POLL_MS <
             MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS,
             "route-ready listener polling must release channel 5 before the next channel-9 retry boundary");
BUILD_ASSERT(MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP <= UWB_MESH_MAX_PAYLOAD_LEN,
             "direct gateway ACK scratch must fit the mesh payload limit");
BUILD_ASSERT(MESH_DIRECT_GATEWAY_ACK_PAYLOAD_CAP >=
                 MESH_ACK_SINGLE_PAYLOAD_LEN,
             "direct gateway ACK scratch must fit exact semantic identity");
BUILD_ASSERT(MESH_EVENT_CONTROL_COMPACT_PAYLOAD_MAX <= UWB_MESH_MAX_PAYLOAD_LEN,
             "event-control retry snapshot must fit the mesh payload limit");
#if DEVICE_ROLE == ROLE_GATEWAY
BUILD_ASSERT(APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN == 192u,
             "production gateway reliable-uplink expansion bound changed");
#endif
BUILD_ASSERT(MESH_ROUTE_EXHAUSTED_RETRY_BASE_MS >= ROUTE_GATEWAY_ACK_TIMEOUT_MS,
             "exhausted route retry must be slower than one ACK timeout");
#define MESH_CH9_DATA_RATE_BPS 850000u
#define MESH_CH9_PHY_OVERHEAD_US 1500u
#define MESH_CH9_TX_FRAME_GAP_MS 2u
#define MESH_GATEWAY_RX_REARM_GUARD_MS \
    APP_MESH_DIRECT_GATEWAY_SERVICE_GUARD_MS
#define MESH_CH9_DIRECT_GATEWAY_TX_GAP_SLOP_MS 5u
#define MESH_CH9_DIRECT_GATEWAY_TX_FRAME_GAP_MS 25u
#define MESH_CH9_TX_CONFIG_GUARD_MS 25u
#define MESH_CH9_TX_SLOT_TRAILER_MS 5u
#define MESH_ROUTE_TEST_CH9_TX_OFFSET_MS MESH_RADIO_EVENT_TX_OFFSET_MS
#define MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS \
    MESH_RADIO_EVENT_RETUNE_GUARD_MS
#define MESH_EVENT_CONTROL_CH5_AIRTIME_MS \
    MESH_RADIO_EVENT_CONTROL_REFERENCE_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_GUARD_MS \
    APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS \
    APP_MESH_DIRECT_GATEWAY_ACK_RX_MS
#define MESH_GATEWAY_DIRECT_PROBE_ACK_RX_SLICE_MS 60u
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
/*
 * A gateway wake train that begins immediately after the direct-probe C5
 * politeness sniff must still be active at the next retry boundary. This
 * gives gateway control a deterministic preemption point even when the first
 * channel-9 probe waits out its entire ACK window.
 */
BUILD_ASSERT(MESH_GATEWAY_DIRECT_PROBE_ATTEMPT_MS +
             (2u * APP_MESH_DIRECT_GATEWAY_ROUTE_BACKOFF_BASE_MS) +
             APP_WAKE_TRAIN_POLITE_SNIFF_MS <
             MESH_RADIO_WAKE_TRAIN_MS,
             "direct gateway retry must recheck C5 inside one wake train");
#define MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS \
    APP_MESH_DIRECT_GATEWAY_ACK_GUARD_MS
/* Fixed delay between a channel-5 report's frame end and the gateway ACK so
 * the sender is always back in RX before the short ACK preamble starts.  A
 * parent anchor answering MSG_MESH_HOP_ACK uses the same turnaround. */
#define MESH_GATEWAY_C5_ACK_GUARD_MS 8u
/*
 * The anchor holding the last delivery slot opens it at
 * window + (n-1) * MESH_CLICK_DELIVERY_SLOT_MS and then waits
 * APP_MESH_PARENT_HOP_ACK_RX_MS for its parent.  The post-burst listen window
 * must still be open when that wait expires, or the last child in the grid is
 * the one child whose parent stopped listening.
 */
BUILD_ASSERT(MESH_CLICK_DELIVERY_SLOT_MS + MESH_CLICK_DELIVERY_LISTEN_TAIL_MS >=
             APP_MESH_PARENT_HOP_ACK_RX_MS,
             "post-burst listen must outlast the last slot's hop ACK wait");
/* Wake the report worker this long before a click delivery slot: the PHY
 * configure guard plus scheduling slack, so the slot boundary is still ahead
 * when the uplink path computes it. */
#define MESH_CLICK_SLOT_WAKE_LEAD_MS (MESH_CH9_TX_CONFIG_GUARD_MS + 5u)
#define MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS \
    (MESH_CH9_TX_CONFIG_GUARD_MS + MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS + \
     MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS)
BUILD_ASSERT(MESH_GATEWAY_RX_REARM_GUARD_MS +
             MESH_GATEWAY_IMMEDIATE_ACK_GUARD_MS +
             MESH_CH9_TX_CONFIG_GUARD_MS <
             MESH_GATEWAY_DIRECT_PROBE_ACK_RX_MS,
             "gateway batch lookahead and immediate ACK turnaround must fit the sender receive window");
#define MESH_DIRECT_GATEWAY_BATCH_TX_WINDOW_MS MESH_EVENT_DEFAULT_WINDOW_MS
#define MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS \
    (MESH_DIRECT_GATEWAY_BATCH_TX_WINDOW_MS + \
     MESH_CH9_DIRECT_GATEWAY_BATCH_ACK_RESERVE_MS)
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
#define MESH_ROUTE_TEST_CH5_STD_PAYLOAD_MAX_LEN \
    (125u - UWB_RF_SCOPE_WIRE_LEN)
#define MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS 20u
#define MESH_ROUTE_WAKE_MAX_TX_PER_RX_CHECK 2u
#define MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS \
    (MESH_ROUTE_WAKE_CLICK_RX_PERIOD_MS + \
     (MESH_ROUTE_WAKE_MAX_TX_PER_RX_CHECK * UWB_CONTROL_TX_TIMEOUT_MS) + \
     ((UWB_CLICKER_WAKE_CLAIM_JITTER_MAX_US + 999u) / 1000u))
/*
 * These are configured transition guards for the two cross-PHY probe
 * reconfigurations, not measured radio timings.  Hardware qualification must
 * still measure both transitions and adjust the guards if the target needs
 * more time.
 */
#define MESH_ROUTE_REPLY_CLICK_PROBE_STANDARD_RETUNE_GUARD_MS \
    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS
#define MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS \
    MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS
#define MESH_ROUTE_REPLY_CLICK_PROBE_BUDGET_MS \
    (MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS + \
     ANCHOR_UWB_WAKE_ACTIVITY_HOLD_MS + \
     MESH_ROUTE_REPLY_CLICK_PROBE_STANDARD_RETUNE_GUARD_MS + \
     MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS)
BUILD_ASSERT(MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS +
                 ANCHOR_UWB_SCAN_ACTIVITY_COMPLETION_MS <=
             ANCHOR_UWB_SCAN_RX_MS +
                 MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS,
             "boosted channel-5 slice must end before the channel-9 prepare horizon");
BUILD_ASSERT(MESH_RADIO_CONTROL_FOLLOWUP_SCAN_MS <=
                 ANCHOR_UWB_SCAN_MESH_RX_RETRY_MS,
             "boosted channel-5 scan must release before its next retry");
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
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS <= 32u,
             "event reciprocal-window ownership must fit its validity mask");
BUILD_ASSERT(MESH_EVENT_ORIGIN_REPLAY_LIFETIME_MS +
                 MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS <=
             INT32_MAX,
             "event replay and queue retention must be wrap-safe");
BUILD_ASSERT(MESH_EVENT_ACCEPT_REPLAY_LIFETIME_MS >=
                 MESH_EVENT_CONTROL_RX_QUEUE_LIFETIME_MS,
             "event replay lifetime must cover queued duplicate control");
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
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS +
                 MESH_EVENT_DEFAULT_WINDOW_MS +
                 MESH_EVENT_RX_LATE_GUARD_MS <=
             MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS,
             "mesh route-test physical RX ownership must fit one half-slot");
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_RETUNE_GUARD_MS +
                 MESH_EVENT_DEFAULT_WINDOW_MS +
                 MESH_EVENT_RX_LATE_GUARD_MS +
                 MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS +
                 MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS <=
             MESH_EVENT_DEFAULT_SECOND_SLOT_OFFSET_MS,
             "two-link relays must retain a complete retune-and-channel-5 scan gap");
BUILD_ASSERT(MESH_DIRECT_GATEWAY_BATCH_WINDOW_MS < ROUTE_GATEWAY_ACK_TIMEOUT_MS,
             "direct gateway batch window must fit inside the ACK timeout");
/* Eight failed scheduled transfers intentionally expire synchronized timing
 * before the longest jittered ACK retry gap. Intentionally empty peer turns
 * advance parity without consuming this failure budget; supervision remains
 * the bounded authority for a completely silent peer. */
BUILD_ASSERT((MESH_EVENT_DEFAULT_MAX_MISSED *
              MESH_EVENT_DEFAULT_INTERVAL_MS) <
             (ROUTE_GATEWAY_ACK_TIMEOUT_MS + MESH_RELAY_RETRY_BACKOFF_MAX_MS),
             "long ACK retry gaps must require channel-9 timing repair");
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
BUILD_ASSERT(APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS <
                 UWB_MESH_GATEWAY_RX_WINDOW_MS,
             "gateway RX work slice must yield within the logical window");
BUILD_ASSERT(APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS > 0u,
             "gateway RX slices must yield the system workqueue");
BUILD_ASSERT(APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS <
                 APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS,
             "gateway RX cooperative yield must remain shorter than a slice");
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
BUILD_ASSERT(MESH_ROUTE_DISCOVERY_MIN_PAYLOAD_LEN >
             MESH_ROUTE_REQ_DISCOVERY_TLV_BYTES,
             "mandatory route ancestry requires the standalone control frame");
BUILD_ASSERT(MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS < WAKE_ADV_MS,
             "route wake TX gaps must leave a click receive opportunity inside one wake train");
BUILD_ASSERT(MESH_ROUTE_WAKE_CLICK_RX_MAX_GAP_MS < WAKE_ADV_MS,
             "a valid repeated click claim must reach the standard-PHY probe");
BUILD_ASSERT(MESH_CONTROL_FOLLOWUP_TURNAROUND_MS >=
             MESH_ROUTE_REPLY_CLICK_PROBE_CONTROL_RETUNE_GUARD_MS,
             "control follow-up must leave time to restore the extended-PHR PHY");
BUILD_ASSERT(MESH_ROUTE_REPLY_LISTEN_WORST_CASE_MS <
             APP_WATCHDOG_PROGRESS_LEASE_MS,
             "worst-case route reply listen must fit inside the watchdog progress lease");
BUILD_ASSERT(APP_MESH_CH9_ACK_BATCH_ENTRY_MAX >= MESH_CH9_TX_BATCH_MAX,
             "mesh route-test ACK batch must cover the largest TX batch");
BUILD_ASSERT(GATEWAY_COMMAND_RESULT_VALIDATION_LEASE_CAP >=
                 MESH_RX_QUEUE_DEPTH + 1u,
             "result validation leases must cover the RX queue plus its consumer");
#if DEVICE_ROLE == ROLE_ANCHOR
BUILD_ASSERT(MESH_CH9_TX_BATCH_MAX <= REPORT_TX_QUEUE_DEPTH,
             "mesh route-test TX batch must fit in the report TX queue");
#if !defined(CONFIG_IMEC_ML_ANCHOR)
BUILD_ASSERT(REPORT_TX_QUEUE_DEPTH >= RANGE_REPORT_MAX_PACKET_FRAGMENTS,
             "one maximum range report must fit in an empty report queue");
BUILD_ASSERT(REPORT_TX_QUEUE_DEPTH >=
                 MESH_CONNECTED_SHARED_RELAY_CLICK_ANCHORS *
                 (ANCHOR_CLICK_RANGE_REPORT_FRAGMENT_CAPACITY +
                  RANGE_REPORT_MAX_CIR_FRAGMENTS),
             "one relay must retain a complete four-anchor click burst");
BUILD_ASSERT(UWB_DEFAULT_CLICK_MAX_EXCHANGES > 0u &&
             UWB_DEFAULT_CLICK_MAX_EXCHANGES <=
                 RANGE_REPORT_MAX_DISTANCE_SAMPLES,
             "default click exchange count must fit the range report model");
BUILD_ASSERT(ANCHOR_CLICK_RANGE_REPORT_FRAGMENT_CAPACITY <=
                 RANGE_REPORT_MAX_PACKET_FRAGMENTS,
             "default click fragment reservation exceeds the report model");
#endif
#endif
#endif

struct mesh_rx_pending {
    struct proto_packet packet;
    uint8_t payload[UWB_MESH_MAX_PAYLOAD_LEN];
    uint16_t payload_len;
    uint64_t previous_hop_id;
    uint64_t first_received_at_ms;
    uint32_t received_at_ms;
    uint32_t result_validation_token;
    struct mesh_event_plan current_channel9_plan;
    uint8_t link_quality;
    int8_t link_rsl_dbm;
    uint8_t radio_channel;
    bool link_rsl_valid;
    bool received_at_valid;
    bool current_channel9_plan_valid;
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

#define MESH_ROUTE_ACTIVATION_RX_SLOTS 4u
struct mesh_route_activation_rx_entry {
    struct mesh_gateway_route_activation activation;
    uint64_t sender_id;
    uint32_t route_adv_start_ms;
    uint32_t relay_window_start_ms;
    uint32_t expires_at_ms;
    bool valid;
};

#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_route_activation_rx_entry
    mesh_route_activation_rx[MESH_ROUTE_ACTIVATION_RX_SLOTS];
#endif

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
    uint64_t remote_boot_nonce;
    uint32_t predecessor_owner_generation;
    struct app_mesh_c5_tx_authorization_token c5_repair_authorization;
    bool replay_existing_response;
    bool predecessor_owner_present;
    bool predecessor_owner_active;
    bool predecessor_owner_from_peer;
    bool replace_local_owner_after_accept;
};

struct mesh_event_local_proposal_windows {
    uint32_t expires_at_ms[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS];
    uint32_t valid_mask;
};

struct mesh_event_accept_completed {
    struct mesh_event_control_record response;
    struct app_mesh_event_request_identity request;
    uint32_t expires_at_ms;
};

BUILD_ASSERT(sizeof(struct mesh_event_accept_completed) == 208u,
             "event ACCEPT completion layout changed; re-audit gateway RAM");

/*
 * An RX ACCEPT is retained only as a bounded duplicate/conflict identity.
 * It never participates in the retry scheduler, so carrying the generic RF
 * retry state here wastes more than 96 bytes of gateway BSS and obscures the
 * actual custody contract.  Keep the full request identity and expiry.
 */
struct mesh_event_accept_rx_cache {
    struct app_mesh_event_request_identity request;
    uint64_t peer_id;
    uint32_t deadline_ms;
    bool valid;
    bool timing_installed;
};

BUILD_ASSERT(sizeof(struct mesh_event_accept_rx_cache) == 64u,
             "event ACCEPT RX cache layout changed; re-audit gateway RAM");

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet);
static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out);
static uint32_t mesh_ch9_estimated_airtime_ms(size_t frame_len);
#if DEVICE_ROLE == ROLE_ANCHOR
#endif

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
#if DEVICE_ROLE == ROLE_ANCHOR
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);

/* In-RAM sequencing state for an anchor range-report batch. Only the
 * fragment identity/order needed for ACK matching and acknowledged-mask
 * accounting is retained; reset discards the batch. */
struct anchor_range_report_control {
    uint64_t clicker_id;
    uint64_t anchor_id;
    uint64_t gateway_id;
    uint32_t event_seq;
    struct {
        uint16_t seq;
        uint8_t digest[SEMANTIC_DIGEST_SHA256_LEN];
    } fragments[RANGE_REPORT_MAX_PACKET_FRAGMENTS];
    uint8_t fragment_count;
    uint8_t attempt_index;
};

struct anchor_range_report_batch_reservation {
    struct anchor_range_report_control control;
    uint64_t clicker_id;
    uint32_t event_seq;
    uint8_t attempt_index;
    uint8_t queued_fragment_count;
    uint8_t fragment_capacity;
    uint8_t queue_prefix_count;
    int queue_error;
    bool queue_admission_fail_closed;
    bool rollback_scratch_owned;
    bool active;
};

static struct anchor_range_report_batch_reservation
    anchor_range_report_batch_reservation;
struct anchor_range_report_ack_runtime {
    struct anchor_range_report_control control;
    uint64_t generation;
    uint16_t acknowledged_mask;
    bool active;
};

static struct anchor_range_report_ack_runtime
    anchor_range_report_ack_runtime;
static uint64_t anchor_range_report_ack_generation;
#endif
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static enum app_mesh_route_wait_tx_owner mesh_route_waiting_tx_owner;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_uwb_rx_work;
static struct k_work mesh_uwb_rx_rearm_work;
static struct k_work_delayable mesh_node_comm_cancel_work;
struct mesh_node_comm_cancel_request {
    struct proto_packet packet;
    uint8_t semantic_digest[SEMANTIC_DIGEST_SHA256_LEN];
    uint32_t delivery_handle;
    uint32_t delivery_generation;
    uint32_t request_token;
    int result;
    bool pending;
    bool complete;
};
static struct mesh_node_comm_cancel_request mesh_node_comm_cancel_request;
static struct k_spinlock mesh_node_comm_cancel_lock;
static struct mesh_delivery_health mesh_delivery_health;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable mesh_route_waiting_work;
#if DEVICE_ROLE == ROLE_ANCHOR
enum mesh_click_preempt_request_state {
    MESH_CLICK_PREEMPT_REQUEST_IDLE = 0,
    MESH_CLICK_PREEMPT_REQUEST_PREPARING,
    MESH_CLICK_PREEMPT_REQUEST_QUEUED,
    MESH_CLICK_PREEMPT_REQUEST_RUNNING,
    MESH_CLICK_PREEMPT_REQUEST_CANCELED,
    MESH_CLICK_PREEMPT_REQUEST_COMPLETE,
};

/* One bounded bridge from the lower-priority click owner to mesh_route. */
struct mesh_click_preempt_request {
    uint32_t generation;
    uint32_t bridge_deadline_ms;
    int result;
    uint8_t state;
};

BUILD_ASSERT(sizeof(struct mesh_click_preempt_request) <= 16u,
             "click-preemption request metadata must stay compact");
BUILD_ASSERT(sizeof(struct mesh_click_preempt_request) +
                 sizeof(struct k_work) +
                 2u * sizeof(struct k_sem) +
                 sizeof(struct k_spinlock) < 112u,
             "click-preemption route bridge exceeds anchor RAM policy margin");

static struct mesh_click_preempt_request mesh_click_preempt_request;
static struct k_work mesh_click_preempt_work;
static struct k_sem mesh_click_preempt_done;
static struct k_sem mesh_click_preempt_route_wake;
static struct k_spinlock mesh_click_preempt_request_lock;
#if defined(CONFIG_ZTEST)
/* Composed seam only: force the otherwise-impossible defensive rollback. */
static void (*mesh_preempt_test_before_cancel_hook)(void);
#endif
static struct k_work_delayable report_tx_work;
static struct k_work_delayable mesh_route_request_action_work;
#endif
static struct k_work_delayable mesh_c5_flood_work;
static struct k_work_delayable mesh_route_discovery_work;
static struct k_work_delayable mesh_event_negotiation_retry_work;
#if DEVICE_ROLE != ROLE_GATEWAY
#define MESH_CH9_CLOSE_INTENT_MAX 2u
struct mesh_ch9_close_intent {
    uint64_t peer_id;
    const char *reason;
    uint32_t retry_due_ms;
    uint32_t owner_session_id;
    uint32_t owner_generation;
    bool upstream;
    bool requires_live_timing;
    bool valid;
};
static struct mesh_ch9_close_intent
    mesh_ch9_close_intents[MESH_CH9_CLOSE_INTENT_MAX];
BUILD_ASSERT(sizeof(mesh_ch9_close_intents) <= 64u,
             "one-upstream/one-downstream close intents must stay compact");
#endif
static K_MUTEX_DEFINE(mesh_uwb_rx_rearm_lock);
static uint32_t mesh_uwb_rx_rearm_delay_ms;
static uint32_t mesh_uwb_rx_rearm_generation;
static bool mesh_uwb_rx_rearm_pending;
/*
 * The gateway continuous channel-5 receive loop drains queued RX inline and
 * can emit a causal ACK from inside that drain.  The generic TX teardown then
 * calls mesh_restart_role_scan(), which finds its own worker busy, defers a
 * rearm and lets that stale work fire behind a loop that is about to arm the
 * receiver itself.  While this owner is set, the running loop guarantees the
 * next arm, so the deferred hop is pure latency and is skipped.
 */
static k_tid_t mesh_inline_rx_rearm_owner;
static struct app_mesh_rf_retry_state mesh_route_request_wake_rf_retry;
static struct app_mesh_rf_retry_state mesh_route_request_control_rf_retry;
static struct app_mesh_rf_retry_state mesh_retransmit_rf_retry;
static struct app_mesh_rf_retry_state mesh_deferred_gateway_ack_rf_retry;
static struct app_mesh_rf_retry_state mesh_route_wait_delivery_rf_retry;
#if DEVICE_ROLE == ROLE_ANCHOR
static uint32_t report_tx_backoff_until_ms;
static bool report_tx_backoff_active;
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
static atomic_t mesh_route_ready_generation;
#if DEVICE_ROLE == ROLE_ANCHOR
static struct app_mesh_c5_control_route_history
    mesh_c5_control_route_history;
#endif
static K_MUTEX_DEFINE(mesh_rx_handler_lock);
static const char *mesh_rx_handler_lock_owner;
static uint32_t mesh_rx_handler_lock_since_ms;
static uint32_t mesh_rx_window_log_next_ms;
static uint32_t mesh_anchor_rx_yield_log_next_ms;
static bool mesh_route_reply_handoff_pending;
static uint32_t mesh_route_reply_handoff_deadline_ms;
struct app_mesh_command_orchestrator *mesh_gateway_command_orchestrator_context(void)
{
    return app_node_comm_gateway_control_context();
}

#if defined(CONFIG_IMEC_DEDICATED_COMM_WORKQUEUE)
K_THREAD_STACK_DEFINE(mesh_route_work_q_stack, MESH_ROUTE_WORKQUEUE_STACK_SIZE);
static struct k_work_q mesh_route_work_q;
static const struct k_work_queue_config mesh_route_work_q_config = {
    .name = "mesh_route",
};
#endif

static const struct app_mesh_report_callbacks *mesh_report_callbacks;

static struct app_mesh_ch9_ack_table mesh_ch9_ack_table;
#include "app_mesh_report_delivery_state.h"
#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_report_delivery_state mesh_report_delivery;
BUILD_ASSERT(sizeof(mesh_report_delivery) <= 4152u,
             "report custody must fit the existing four-packet bank");
#endif
static uint64_t mesh_c5_rx_burst_peer;
static int mesh_report_delivery_step(void);
static bool mesh_report_delivery_active(void);
#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_report_delivery_contains(const struct mesh_outbound *out);
#endif
static void mesh_c5_update_credit(void);
static bool mesh_c5_collect_ack(const struct mesh_outbound *ack, const struct mesh_rx_pending *rx);
static void mesh_c5_capture_followers(const struct mesh_outbound *ack, const struct mesh_rx_pending *rx);
static void mesh_c5_flush_batch_ack(void);

#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_anchor_downlink_store mesh_anchor_downlink_store;
BUILD_ASSERT(MESH_CONNECTED_ANCHOR_REPORT_RECOVERY_RESERVE_CAPACITY == 1u,
             "report queue ownership has exactly one recovery reserve");
BUILD_ASSERT(sizeof(mesh_anchor_downlink_store) == 1672u,
             "anchor downlink and route ancestry RAM contract changed");
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(sizeof(struct mesh_outbound) == 1024u,
             "connected anchor outbound record RAM contract changed");
#endif
#elif DEVICE_ROLE == ROLE_GATEWAY
static struct mesh_gateway_ack_store mesh_gateway_ack_store;
static bool mesh_gateway_ack_store_initialized;
static bool mesh_gateway_ack_store_attached;
BUILD_ASSERT(sizeof(mesh_gateway_ack_store) == 5384u,
             "gateway ACK store RAM contract changed");
#endif

int app_mesh_report_attach_gateway_ack_store(void)
{
#if DEVICE_ROLE == ROLE_GATEWAY
    int ret;

    if (!mesh_gateway_ack_store_initialized) {
        return -EACCES;
    }
    if (mesh_gateway_ack_store_attached) {
        return -EALREADY;
    }

    ret = mesh_relay_attach_gateway_ack_store(&mesh_runtime,
                                              &mesh_gateway_ack_store);
    if (ret != PROTO_OK) {
        return -EIO;
    }
    mesh_gateway_ack_store_attached = true;
    return 0;
#else
    return -ENOTSUP;
#endif
}

bool app_mesh_report_gateway_delivery_confirmation_pending(
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms)
{
    return mesh_relay_gateway_delivery_confirmation_pending(
        &mesh_runtime, src_id, msg_type, session_id, now_ms);
}

bool app_mesh_report_gateway_identity_confirmation_pending(
    uint64_t src_id,
    uint8_t msg_type,
    uint32_t session_id,
    uint16_t seq,
    uint32_t now_ms)
{
    return mesh_relay_gateway_identity_confirmation_pending(
        &mesh_runtime, src_id, msg_type, session_id, seq, now_ms);
}

bool app_mesh_report_gateway_operation_confirmation_pending(
    uint8_t msg_type,
    uint32_t session_id,
    uint32_t now_ms)
{
    return mesh_relay_gateway_operation_confirmation_pending(
        &mesh_runtime, msg_type, session_id, now_ms);
}

bool app_mesh_report_gateway_origin_confirmation_pending(uint64_t src_id,
                                                         uint32_t now_ms)
{
    return mesh_relay_gateway_origin_confirmation_pending(
        &mesh_runtime, src_id, now_ms);
}

int app_mesh_report_attach_anchor_downlink_store(void)
{
#if DEVICE_ROLE == ROLE_ANCHOR
    int ret;

    if (mesh_runtime.anchor_downlink_store != NULL) {
        return -EALREADY;
    }
    ret = mesh_relay_attach_anchor_downlink_store(
        &mesh_runtime, &mesh_anchor_downlink_store);
    return ret == PROTO_OK ? 0 : -EIO;
#else
    return -ENOTSUP;
#endif
}

static K_MUTEX_DEFINE(mesh_send_scratch_lock);
#if DEVICE_ROLE == ROLE_ANCHOR
/*
 * Anchor queue commit/remove helpers need a complete mutable envelope while
 * holding mesh_send_scratch_lock. Gateway and clicker TX only borrow their
 * synchronous caller's immutable payload, so they must not pay this 1 KiB
 * BSS cost.
 */
static struct mesh_outbound mesh_send_scratch_tx;
#endif
static uint8_t mesh_send_scratch_frame[UWB_MESH_MAX_FRAME_LEN];
#if DEVICE_ROLE == ROLE_ANCHOR

#endif

enum mesh_radio_release_policy {
    MESH_RADIO_RELEASE_STANDBY,
    MESH_RADIO_RELEASE_IDLE,
};

#if DEVICE_ROLE == ROLE_ANCHOR
static struct mesh_outbound report_tx_worker_scratch;
static struct mesh_outbound report_tx_queue_overflow_dropped;
static struct mesh_outbound report_tx_queue_rotation_scratch;
static bool report_tx_queue_recovery_valid;
static struct app_mesh_queue_head_owner report_tx_queue_head_owner;
#endif
static struct mesh_relay_result mesh_work_result;
static struct mesh_outbound mesh_tx_timeout_pending_waiting;
static struct mesh_rx_pending mesh_rx_work_pending;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
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
static struct fw_delivery_loss_state mesh_delivery_loss;
#if DEVICE_ROLE == ROLE_ANCHOR
static K_MUTEX_DEFINE(report_tx_queue_overflow_lock);
#endif
static K_MUTEX_DEFINE(mesh_c5_control_scratch_lock);
static struct k_spinlock mesh_rx_handoff_lock;
static struct app_mesh_rx_handoff_state mesh_rx_handoff;
/*
 * An enumeration control burst is one radio operation even though it has
 * several physical copies. Keep a reference count because a duplicate can
 * be admitted while the first copy set is still completing; a boolean would
 * let the first finisher reopen the scanner.
 */
static atomic_t mesh_c5_enumeration_relay_burst_count;
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
static K_MUTEX_DEFINE(mesh_route_discovery_lock);
static struct app_mesh_async_route_request mesh_route_discovery_request;
struct app_mesh_route_ready_event_owner {
    bool valid;
    uint64_t peer_id;
    uint32_t generation;
};

static struct app_mesh_route_ready_event_owner mesh_route_ready_event_owner;

static void mesh_route_ready_event_owner_set(
    struct app_mesh_route_ready_event_owner *owner,
    uint64_t peer_id,
    uint32_t generation)
{
    if (owner == NULL || !mesh_id_is_unicast(peer_id) || generation == 0u) {
        return;
    }
    owner->peer_id = peer_id;
    owner->generation = generation;
    owner->valid = true;
}

static bool mesh_route_ready_event_owner_matches(
    const struct app_mesh_route_ready_event_owner *owner,
    uint64_t peer_id,
    uint32_t generation)
{
    return owner != NULL && owner->valid && owner->peer_id == peer_id &&
           owner->generation == generation;
}

static void mesh_route_ready_event_owner_clear(void)
{
    memset(&mesh_route_ready_event_owner, 0,
           sizeof(mesh_route_ready_event_owner));
}
static struct mesh_event_control_record mesh_event_propose_record;
static struct app_mesh_event_retry_state mesh_event_propose_retry;
static struct app_mesh_known_parent_contact_retry_state
    mesh_known_parent_contact_retry;
static bool mesh_event_propose_topology_operation;
static struct app_mesh_c5_tx_authorization_token
    mesh_forwarded_ack_event_repair_authorization;
static struct app_mesh_c5_tx_authorization_token
    mesh_deferred_forwarded_ack_event_repair_authorization;
static struct mesh_event_accept_retry_context mesh_event_accept_retry;
static struct mesh_event_accept_completed
    mesh_event_accept_completed[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS];
static uint8_t mesh_event_accept_completed_cursor;
static struct mesh_event_accept_rx_cache mesh_event_accept_rx_cache;
static struct mesh_event_owner
    mesh_event_owners[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS];
static struct mesh_event_origin_tombstone
    mesh_event_origin_tombstones[MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS];
static const struct mesh_event_owner_registry mesh_event_owner_registry = {
    .owners = mesh_event_owners,
    .owner_capacity = ARRAY_SIZE(mesh_event_owners),
    .tombstones = mesh_event_origin_tombstones,
    .tombstone_capacity = ARRAY_SIZE(mesh_event_origin_tombstones),
};
static struct mesh_event_local_proposal_windows
    mesh_event_local_proposal_windows;
static uint32_t mesh_event_operation_session_next;
static K_MUTEX_DEFINE(mesh_event_control_retry_scratch_lock);
static struct mesh_outbound mesh_event_control_retry_scratch;
static uint32_t mesh_direct_gateway_bulk_suppressed_until_ms;
static bool mesh_direct_gateway_bulk_suppressed;
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
struct mesh_c5_flood_deferred_entry {
    bool valid;
    uint32_t generation;
    struct mesh_outbound outbound;
    uint8_t purpose;
    const char *reason;
    bool response_priority;
    uint8_t retry_count;
    struct app_mesh_rf_retry_state rf_retry;
    uint32_t queued_at_ms;
};

static struct mesh_c5_flood_deferred_entry mesh_c5_flood_deferred;
/*
 * A committed route-advertisement watermark cannot be rolled back merely
 * because the general response-priority slot is occupied. Keep one separate,
 * replaceable owner for the newest committed advertisement; later sequences
 * supersede older unsent route advertisements without weakening freshness.
 */
static struct mesh_c5_flood_deferred_entry mesh_route_adv_deferred;
static K_MUTEX_DEFINE(mesh_c5_flood_deferred_lock);

#define MESH_C5_DEFERRED_MAX_RETRIES 8u
#define MESH_C5_LOCAL_DEFER_RETRY_MS 5u
#define MESH_C5_LOCAL_DEFER_MAX_AGE_MS \
    NODE_COMM_BOUNDED_CONTROL_HOP_BUDGET_MS

struct mesh_c5_flood_tx_context {
    bool *rf_started_out;
    struct app_mesh_tx_observation *observation;
    uint64_t absolute_deadline_ms;
    bool response_priority;
    bool atomic_gateway_control;
    uint8_t c5_tx_intent;
    const struct mesh_outbound *candidate;
};

void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
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
                                            const struct app_mesh_async_route_transfer_identity *route_transfer,
                                            bool *send_attempted,
                                            bool *rf_sent,
                                            bool *policy_deferred,
                                            bool retain_initial_channel9_wait,
                                            uint64_t absolute_deadline_ms,
                                            struct app_mesh_tx_observation *observation);
static int mesh_schedule_tx_timeout(void);
static void mesh_route_discovery_work_handler(struct k_work *work);
static void mesh_event_negotiation_retry_work_handler(struct k_work *work);
static int mesh_event_negotiation_schedule_next(void);
static bool mesh_channel9_close_intent_blocks_upstream(uint64_t peer_id);
#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_event_propose_prepare_immediate_send(
    const struct mesh_outbound *outbound);
#endif
static int mesh_radio_idle_with_bounded_recovery(const char *reason);
static int mesh_radio_standby_with_bounded_recovery(const char *reason);
int mesh_schedule_route_request(uint64_t target_id, const char *reason);
static int mesh_request_route_authorized(
    uint64_t target_id,
    const char *reason,
    const struct app_mesh_c5_tx_authorization_token *authorization);
static bool mesh_defer_active_collection_result(const char *reason);
static bool mesh_channel9_next_required_activity(
    const struct mesh_relay_event_timing_entry *entry,
    const uint64_t *local_reliable_tx_peers,
    size_t local_reliable_tx_peer_count,
    struct mesh_event_timing *timing);
static uint32_t mesh_channel9_prepare_start_ms(const struct mesh_event_timing *timing);
static int mesh_schedule_uwb_rx(uint32_t delay_ms);
#if DEVICE_ROLE == ROLE_ANCHOR
static int mesh_preempt_transfer_click_report_atomic(
    void *ctx,
    const struct mesh_outbound *outbound);
static int mesh_preempt_defer_active_tx(void *ctx);
static int mesh_preempt_schedule_timeout(void *ctx);
static void mesh_preempt_fail_stop(void *ctx);
static void mesh_click_preempt_work_handler(struct k_work *work);
static void mesh_click_preempt_request_init(void);
#endif
#if DEVICE_ROLE == ROLE_ANCHOR
static void mesh_handoff_note_result_bundle_forwarded(const struct mesh_outbound *out,
                                                      void *ctx);
#endif
static int mesh_handoff_send_result_grant(const struct mesh_outbound *out,
                                          void *ctx);
static void mesh_handoff_note_tx_sent(const struct mesh_outbound *out,
                                      void *ctx);
static int mesh_send_outbound_causal_response(
    const struct mesh_outbound *out,
    const char *reason);
static int mesh_send_c5_causal_response(
    const struct mesh_outbound *out,
    uint8_t purpose,
    enum mesh_c5_control_send_mode mode,
    const char *reason);
static int mesh_send_route_wake_train(uint64_t target_id,
                                      const struct mesh_outbound *embedded_route_req,
                                      bool *embedded_sent,
                                      uint8_t purpose,
                                      const char *reason,
                                      const struct mesh_outbound *authorization_candidate,
                                      const struct app_mesh_c5_tx_authorization_token *authorization,
                                      enum fw_c5_tx_intent c5_tx_intent,
                                      bool *rf_started_out,
                                      uint64_t *rf_started_at_ms_out);
static int mesh_send_route_wake_train_with_duration(
    uint64_t target_id,
    const struct mesh_outbound *embedded_route_req,
    bool *embedded_sent,
    uint8_t purpose,
    const char *reason,
    const struct mesh_outbound *authorization_candidate,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    enum fw_c5_tx_intent c5_tx_intent,
    uint32_t wake_train_ms,
    bool *rf_started_out,
    uint64_t *rf_started_at_ms_out);
static bool mesh_frame_requires_anchor_click_handoff(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim);
static bool mesh_handoff_anchor_click_claim(
    const struct uwb_wake_claim_frame *claim,
    uint8_t quality,
    uint32_t observed_packet_ms);
static int mesh_probe_standard_wake_claim(
    uint8_t *frame,
    size_t frame_cap,
    struct uwb_wake_claim_frame *click_claim,
    uint8_t *click_quality,
    uint32_t *click_observed_ms,
    bool allow_relayed_gateway_control,
    int64_t deadline_ms);
static int mesh_send_c5_flood_now(const struct mesh_outbound *out,
                                  uint8_t purpose,
                                  const char *reason,
                                  bool send_wake_train,
                                  bool response_priority,
                                  bool single_opportunity,
                                  const struct app_mesh_command_orchestrator *command_orchestrator,
                                  struct app_mesh_flood_result *result,
                                  bool *rf_started_out);
static int mesh_send_c5_flood_now_intent(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    bool send_wake_train,
    bool response_priority,
    bool single_opportunity,
    const struct app_mesh_command_orchestrator *command_orchestrator,
    struct app_mesh_flood_result *result,
    bool *rf_started_out,
    enum fw_c5_tx_intent c5_tx_intent);
static int mesh_send_c5_flood_now_until(
    const struct mesh_outbound *out,
    uint8_t purpose,
    const char *reason,
    bool send_wake_train,
    bool response_priority,
    bool single_opportunity,
    const struct app_mesh_command_orchestrator *command_orchestrator,
    struct app_mesh_flood_result *result,
    bool *rf_started_out,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation,
    enum fw_c5_tx_intent c5_tx_intent);
static bool mesh_c5_enumeration_relay_burst_active(void);
static bool mesh_c5_gateway_enumeration_quick_copy_burst(
    const struct mesh_outbound *out);
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
static bool mesh_decode_channel5_wake_claim(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim,
    bool *embedded_route_frame_out);
static bool mesh_decode_gateway_route_activation_frame(
    const uint8_t *frame,
    size_t frame_len,
    struct uwb_wake_claim_frame *claim,
    struct mesh_gateway_route_activation *activation);
static bool mesh_route_activation_allows_route_adv(
    uint64_t sender_id,
    uint32_t gateway_route_seq,
    uint8_t ttl,
    uint32_t received_at_ms);
static bool mesh_route_activation_apply_forward_timing(
    struct mesh_outbound *route_adv,
    const struct mesh_rx_pending *rx);
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
static bool mesh_channel9_close_intent_ready_for_peer(uint64_t peer_id);
static bool mesh_select_channel9_close_tx_event(
    uint32_t now_ms,
    struct mesh_event_plan *selected_plan,
    uint64_t *selected_peer);
static int mesh_send_pending_channel9_close(
    const struct mesh_event_plan *plan,
    uint64_t peer_id);
static void mesh_event_owner_abandon_peer(uint64_t peer_id);
static struct mesh_event_owner *mesh_event_owner_for_peer(uint64_t peer_id);
static uint32_t mesh_parent_contact_retry_delay_ms(uint64_t parent_id,
                                                   uint8_t failure_count);
static int mesh_send_pending_ch9_ack_batch(const struct mesh_event_plan *plan,
                                           uint64_t peer_id,
                                           const char *reason);
#if DEVICE_ROLE == ROLE_ANCHOR
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
#endif
static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *out,
                                                       const char *reason,
                                                       size_t *frame_len_out);
static int mesh_send_outbound_preconfigured_ch9_locked_until(
    const struct mesh_outbound *out,
    const char *reason,
    size_t *frame_len_out,
    uint64_t absolute_deadline_ms,
    struct app_mesh_tx_observation *observation);
static void mesh_wait_until_ms(uint32_t target_ms);
static int mesh_prepare_event_timing(struct mesh_event_timing *timing, uint32_t now_ms);
static uint32_t mesh_route_test_first_event_time_ms(uint32_t now_ms);
static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms);
static int mesh_cancel_delayable(struct k_work_delayable *work);
static int mesh_reschedule_owned_work(struct k_work_delayable *work,
                                      uint32_t delay_ms,
                                      const char *owner);
static int mesh_submit_owned_work(struct k_work *work, const char *owner);
static bool mesh_transport_paused(void);
static bool mesh_rx_handoff_control_active(void);
static int mesh_propose_event_after_channel5_contact(uint64_t peer_id,
                                                     const char *reason);
static int mesh_propose_event_after_channel5_contact_authorized(
    uint64_t peer_id,
    const char *reason,
    const struct app_mesh_c5_tx_authorization_token *authorization,
    bool topology_operation,
    bool assignment_response);
static void mesh_uwb_rx_rearm_work_handler(struct k_work *work);
static void mesh_node_comm_cancel_work_handler(struct k_work *work);
static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
                                     uint32_t protocol_validation_token,
                                     const struct mesh_event_plan *current_channel9_plan,
                                     uint64_t current_channel9_peer_id,
                                     bool *valid_mesh_frame,
                                     uint64_t *previous_hop_id);
static bool mesh_queue_from_frame_at_internal(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t link_quality,
    int8_t link_rsl_dbm,
    bool link_rsl_valid,
    uint8_t radio_channel,
    uint32_t received_at_ms,
    uint32_t protocol_validation_token,
    const struct mesh_event_plan *current_channel9_plan,
    uint64_t current_channel9_peer_id,
    bool submit_work,
    bool *valid_mesh_frame,
    uint64_t *previous_hop_id,
    uint8_t *packet_flags_out);
static uint64_t mesh_expand_uptime32(uint32_t timestamp_ms);
#if DEVICE_ROLE == ROLE_ANCHOR
static bool mesh_outbound_is_local_origin_priority(
    const struct mesh_outbound *out);
#endif
static void mesh_event_accept_rx_clear_peer(uint64_t peer_id);
static void mesh_event_accept_completed_clear_peer(uint64_t peer_id);
static void mesh_retire_assignment_channel9_peer(uint64_t peer_id,
                                                 const char *reason);

/* Implementation is split by responsibility but remains one translation unit. */
#include "app_mesh_report_coordination.inc"
#include "app_mesh_report_transport.inc"
#include "app_mesh_report_route_control.inc"
#include "app_mesh_report_direct_gateway.inc"
#include "app_mesh_report_event_tx.inc"
#include "app_mesh_report_delivery.inc"
#include "app_mesh_report_rx.inc"
#include "app_mesh_report_c5_batch.inc"

#if defined(CONFIG_IMEC_STACK_DIAGNOSTICS)
/*
 * Stack evidence replay must never run between a frame decode and its RX
 * re-arm, or while a hop ACK is still on the air: the sampler walks every
 * thread stack and blocks on RTT buffer space. These three reads are the
 * cheapest live radio-path state this translation unit already owns.
 */
static bool mesh_stack_diag_radio_critical(void)
{
    return mesh_inline_rx_rearm_owner != NULL ||
           app_mesh_rx_handoff_control_active(&mesh_rx_handoff) ||
           mesh_relay_tx_active(&mesh_runtime);
}

static int mesh_stack_diag_hook_init(void)
{
    app_stack_workload_diag_set_critical_hook(mesh_stack_diag_radio_critical);
    return 0;
}

SYS_INIT(mesh_stack_diag_hook_init, APPLICATION, 99);
#endif
