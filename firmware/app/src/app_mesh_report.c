#include "app_mesh_report.h"

#include "app_board.h"
#include "app_clicker.h"
#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_test.h"
#include "app_state.h"
#include "dwm3000_driver.h"
#include "mesh.h"
#include "mesh_relay.h"
#include "protocol.h"
#include "report.h"
#include "route.h"
#include "uwb.h"
#include "uwb_session.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_report, LOG_LEVEL_DBG);

#define MESH_ROUTE_TEST_WAKE_TO_ROUTE_DELAY_MS 40u
#define MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS 50u
#define MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_COUNT 2u
#define MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS 25u
#define MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS 80u
#define MESH_ROUTE_TEST_REPLY_HANDOFF_WAIT_MS 250u
#define MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS 1000u
#define MESH_ROUTE_TEST_REPLY_CAPTURE_MAX 4u
#define MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS 50u
#define MESH_ROUTE_WAIT_RX_SUPPRESS_MS 100u
#define MESH_RX_WINDOW_IDLE_LOG_INTERVAL_MS 1000u
#define MESH_CH9_ACK_BATCH_MAX 8u
#define MESH_CH9_TX_BATCH_MAX 8u
#define MESH_CH9_DATA_RATE_BPS 850000u
#define MESH_CH9_PHY_OVERHEAD_US 1500u
#define MESH_CH9_TX_FRAME_GAP_MS 2u
#define MESH_CH9_TX_CONFIG_GUARD_MS 25u
#define MESH_CH9_TX_SLOT_TRAILER_MS 5u
#define MESH_ROUTE_TEST_CH9_TX_OFFSET_MS 20u
#define MESH_EVENT_CONTROL_CH5_AIRTIME_MS 10u
#define MESH_ROUTE_TEST_CH5_GAP_SCAN_MS 100u
#define MESH_GATEWAY_CH5_CONTINUOUS_RX_MS 2000u
#define MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS 20u
#define MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS MESH_EVENT_DEFAULT_GUARD_MS
#define MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS 2u
#define MESH_GATEWAY_ROUTE_PREEMPT_MS 2000u
#define MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS 10u

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
BUILD_ASSERT(MESH_RELAY_EVENT_TIMINGS >= MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
             "mesh route-test needs room for upstream and downstream channel-9 timings");
BUILD_ASSERT(MESH_ROUTE_TEST_CH9_TX_OFFSET_MS + MESH_CH9_TX_SLOT_TRAILER_MS <
             MESH_EVENT_DEFAULT_WINDOW_MS,
             "mesh route-test TX offset must fit inside the channel-9 window");
BUILD_ASSERT(MESH_CH9_ACK_BATCH_MAX >= MESH_CH9_TX_BATCH_MAX,
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

K_MSGQ_DEFINE(mesh_rx_msgq, sizeof(struct mesh_rx_pending), MESH_RX_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(report_tx_msgq, sizeof(struct mesh_outbound), REPORT_TX_QUEUE_DEPTH, 4);
static struct mesh_outbound mesh_route_waiting_tx;
static bool mesh_route_waiting_tx_valid;
static struct k_work mesh_rx_work;
static struct k_work_delayable mesh_uwb_rx_work;
static struct k_work_delayable mesh_tx_timeout_work;
static struct k_work_delayable report_tx_work;
static uint32_t mesh_rx_window_log_next_ms;
static bool mesh_route_reply_handoff_pending;
static uint32_t mesh_route_reply_handoff_deadline_ms;

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
K_THREAD_STACK_DEFINE(mesh_route_work_q_stack, MESH_ROUTE_WORKQUEUE_STACK_SIZE);
static struct k_work_q mesh_route_work_q;
static const struct k_work_queue_config mesh_route_work_q_config = {
    .name = "mesh_route",
};
#endif

static const struct app_mesh_report_callbacks *mesh_report_callbacks;

struct mesh_ch9_ack_entry {
    uint16_t seq;
    uint32_t session_id;
    uint32_t packet_id;
    bool has_packet_id;
};

struct mesh_ch9_ack_batch {
    struct mesh_outbound template_ack;
    struct mesh_ch9_ack_entry entries[MESH_CH9_ACK_BATCH_MAX];
    uint8_t count;
    bool valid;
};

static struct mesh_ch9_ack_batch mesh_ch9_ack_batch;

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
static K_MUTEX_DEFINE(mesh_send_scratch_lock);
static struct mesh_outbound mesh_send_scratch_tx;
static uint8_t mesh_send_scratch_frame[UWB_MESH_MAX_FRAME_LEN];

struct mesh_ch9_slot_tx_context {
    int64_t uwb_window_start_ms;
    bool active;
};

static struct mesh_outbound report_tx_batch_first;
static struct mesh_outbound report_tx_batch_queued;
static struct mesh_outbound report_tx_batch_tx;
static struct mesh_outbound report_tx_batch_dropped;
static struct mesh_outbound report_tx_work_outbound;
static struct mesh_outbound report_tx_work_dropped;
static struct mesh_relay_result mesh_work_result;
static struct mesh_outbound mesh_tx_timeout_pending_waiting;
static struct mesh_outbound mesh_tx_timeout_pending_report;
static struct mesh_rx_pending mesh_rx_work_pending;
static uint8_t mesh_uwb_rx_frame[UWB_MESH_MAX_FRAME_LEN];
static struct mesh_outbound mesh_result_action_tx;
static uint64_t mesh_route_ready_event_peer_id;
static uint64_t mesh_gateway_route_preempt_peer_id;
static uint32_t mesh_gateway_route_preempt_deadline_ms;

static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements);
static void mesh_try_route_waiting_tx(void);
static int mesh_send_route_wake_train(uint64_t target_id, const char *reason);
static int mesh_listen_for_route_reply(uint64_t target_id,
                                       const char *reason,
                                       bool *route_reply_captured);
static bool mesh_packet_is_event_control_type(uint8_t msg_type);
static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t link_quality);
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
static bool mesh_ch9_tx_fits_configured_slot(const struct mesh_outbound *out,
                                             const struct mesh_event_plan *plan,
                                             uint32_t now_ms,
                                             uint32_t send_start_ms,
                                             uint32_t *required_ms);
static size_t mesh_outbound_encoded_frame_len(const struct mesh_outbound *out);
static uint32_t mesh_ch9_slot_send_start_ms(const struct mesh_outbound *out,
                                            const struct mesh_event_plan *plan,
                                            uint32_t now_ms);
static bool mesh_ch9_tx_fits_plan(const struct mesh_outbound *out,
                                  const struct mesh_event_plan *plan,
                                  uint32_t now_ms,
                                  uint32_t *required_ms);
static int mesh_ch9_slot_tx_begin(struct mesh_ch9_slot_tx_context *ctx);
static void mesh_ch9_slot_tx_end(struct mesh_ch9_slot_tx_context *ctx);
static int mesh_send_outbound_preconfigured_ch9_locked(const struct mesh_outbound *out,
                                                       const char *reason,
                                                       size_t *frame_len_out);
static void mesh_wait_until_ms(uint32_t target_ms);
static uint32_t mesh_route_test_first_event_time_ms(uint32_t now_ms);
static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms);
static int mesh_cancel_delayable(struct k_work_delayable *work);
static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
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

static int mesh_install_channel9_timing(uint64_t peer_id,
                                        const struct mesh_event_timing *timing,
                                        const char *reason)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        if (DEVICE_ROLE == ROLE_GATEWAY) {
            int ret;

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
            if (mesh_gateway_route_test_slots_full_for(peer_id)) {
                status_debug_note("DBG_GATEWAY_CH9_FULL\n");
                status_debug_printf("DBG_GATEWAY_CH9_FULL peer=0x%llx active=%u reason=%s\n",
                                    (unsigned long long)peer_id,
                                    mesh_channel9_connection_count(),
                                    reason == NULL ? "install" : reason);
                LOG_WRN("mesh gateway rejected channel-9 timing: two upstream slots already active peer=0x%016llx reason=%s",
                        (unsigned long long)peer_id,
                        reason == NULL ? "install" : reason);
                return PROTO_ERR_NO_SPACE;
            }

            ret = mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, timing);
            if (ret == PROTO_OK) {
                status_debug_note("DBG_GATEWAY_CH9_UPSTREAM_INSTALLED\n");
                status_debug_printf("DBG_GATEWAY_CH9_UPSTREAM peer=0x%llx active=%u cnt=%u next=%u\n",
                                    (unsigned long long)peer_id,
                                    mesh_channel9_connection_count(),
                                    timing->event_counter,
                                    timing->next_event_time_ms);
            }
            return ret;
        }
        struct mesh_relay_channel9_guard_status guard = {0};
        int ret = mesh_relay_set_channel9_timing_guarded(&mesh_runtime,
                                                         peer_id,
                                                         timing,
                                                         MESH_ROUTE_TEST_CH9_MAX_CONNECTIONS,
                                                         &guard);

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
    ARG_UNUSED(reason);
#endif

    return mesh_relay_set_channel9_timing(&mesh_runtime, peer_id, timing);
}

static bool mesh_ch9_ack_batch_clear_for_peer(uint64_t peer_id,
                                              const char *reason)
{
    uint8_t count;
    uint16_t first_seq;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !mesh_id_is_unicast(peer_id) ||
        !mesh_ch9_ack_batch.valid ||
        mesh_ch9_ack_batch.template_ack.next_hop_id != peer_id) {
        return false;
    }

    count = mesh_ch9_ack_batch.count;
    first_seq = count == 0u ? 0u : mesh_ch9_ack_batch.entries[0].seq;
    status_debug_printf("DBG_CH9_ACK_CLEAR peer=0x%llx n=%u first=%u reason=%s\n",
                        (unsigned long long)peer_id,
                        count,
                        first_seq,
                        reason == NULL ? "clear" : reason);
    memset(&mesh_ch9_ack_batch, 0, sizeof(mesh_ch9_ack_batch));
    return true;
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

static void mesh_report_gateway_handle_survey_discovery_report(
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len)
{
    if (mesh_report_callbacks != NULL &&
        mesh_report_callbacks->gateway_handle_survey_discovery_report != NULL) {
        mesh_report_callbacks->gateway_handle_survey_discovery_report(packet, payload, payload_len);
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

int anchor_append_sequence_time_tlvs(uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *payload_len,
                                            int64_t local_ms)
{
    uint64_t timestamp_ms = 0u;

    anchor_sequence_timestamp_at(local_ms, &timestamp_ms);

    return tlv_append_u64(payload,
                          payload_cap,
                          payload_len,
                          TLV_TIMESTAMP_MS,
                          timestamp_ms);
}

static bool range_result_has_raw_timestamps(const struct dwm3000_range_result *result)
{
    return result != NULL &&
           (result->poll_tx_ts_32 != 0u ||
            result->poll_rx_ts_32 != 0u ||
            result->resp_tx_ts_32 != 0u ||
            result->resp_rx_ts_32 != 0u ||
            result->final_tx_ts_32 != 0u ||
            result->final_rx_ts_32 != 0u);
}

int append_range_result_timing_tlvs(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    const struct dwm3000_range_result *result)
{
    const uint8_t *rx_diag = NULL;
    uint8_t rx_diag_len = 0u;
    int ret;

    if (payload == NULL || payload_len == NULL || result == NULL) {
        return PROTO_ERR_ARG;
    }

    if (result->rsl_sampled) {
        ret = tlv_append_i8(payload, payload_cap, payload_len,
                            TLV_UWB_RSL_DBM, result->rsl_dbm);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->clock_offset_sampled) {
        ret = tlv_append_u16(payload, payload_cap, payload_len,
                             TLV_UWB_CLOCK_OFFSET_RAW,
                             (uint16_t)result->clock_offset_raw);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (result->carrier_integrator_sampled) {
        ret = tlv_append_i32(payload, payload_cap, payload_len,
                             TLV_UWB_CARRIER_INTEGRATOR,
                             result->carrier_integrator);
        if (ret != PROTO_OK) {
            return ret;
        }
    }
    if (range_result_has_raw_timestamps(result)) {
        uint8_t timestamps[6u * sizeof(uint32_t)];

        proto_put_u32_le(&timestamps[0], result->poll_tx_ts_32);
        proto_put_u32_le(&timestamps[4], result->poll_rx_ts_32);
        proto_put_u32_le(&timestamps[8], result->resp_tx_ts_32);
        proto_put_u32_le(&timestamps[12], result->resp_rx_ts_32);
        proto_put_u32_le(&timestamps[16], result->final_tx_ts_32);
        proto_put_u32_le(&timestamps[20], result->final_rx_ts_32);
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RAW_TIMESTAMPS,
                               timestamps,
                               sizeof(timestamps));
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    if (result->clicker_rx_diag_sampled) {
        rx_diag = result->clicker_rx_diag_raw;
        rx_diag_len = result->clicker_rx_diag_raw_len;
    } else if (result->anchor_rx_diag_sampled) {
        rx_diag = result->anchor_rx_diag_raw;
        rx_diag_len = result->anchor_rx_diag_raw_len;
    }
    if (rx_diag != NULL && rx_diag_len > 0u) {
        ret = tlv_append_bytes(payload, payload_cap, payload_len,
                               TLV_UWB_RX_DIAG_BYTES,
                               rx_diag,
                               rx_diag_len);
        if (ret != PROTO_OK) {
            return ret;
        }
    }

    return PROTO_OK;
}

static uint32_t anchor_status_bits(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0u;
    }

    return uwb_session_status_bits_from_diagnostics(&anchor_uwb_session.diagnostics);
}

int append_anchor_status_tlvs(uint8_t *payload, size_t payload_cap, size_t *payload_len)
{
    struct anchor_heartbeat_fields fields = {
        .device_role = (uint8_t)DEVICE_ROLE,
        .battery_mv = ANCHOR_BATTERY_MV_UNKNOWN,
        .status_bits = anchor_status_bits(),
        .uptime_ms = k_uptime_get_32(),
    };
    int ret;

    anchor_sequence_timestamp_at(k_uptime_get(), &fields.timestamp_ms);
    ret = report_append_anchor_heartbeat_tlvs(payload, payload_cap, payload_len, &fields);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = mesh_relay_append_status_tlvs(&mesh_runtime, payload, payload_cap, payload_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL_SWITCHES,
                         mesh_event_stats.channel_switches);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_PLL_READY_FAILURES,
                         mesh_event_stats.pll_ready_failures);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_LATE_CHANNEL5_RETURNS,
                         mesh_event_stats.late_channel5_returns);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_DEFERRALS,
                         mesh_event_stats.mesh_deferrals);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CH9_EVENT_MISSES,
                         mesh_event_stats.ch9_event_misses);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload,
                         payload_cap,
                         payload_len,
                         TLV_MESH_CHANNEL5_PREEMPTIONS,
                         mesh_event_stats.channel5_preemptions);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u32(payload,
                          payload_cap,
                          payload_len,
                          TLV_MESH_CH9_REPORT_LATENCY_MS,
                          mesh_event_stats.ch9_report_latency_ms);
}

void mesh_preempt_for_click_event(void)
{
    struct mesh_outbound pending_report = {0};
    bool requeue_report = false;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    if (mesh_relay_tx_active(&mesh_runtime)) {
        if (mesh_runtime.pending.packet.msg_type == MSG_CLICK_REPORT &&
            mesh_runtime.pending.packet.src_id == DEVICE_ID) {
            pending_report.packet = mesh_runtime.pending.packet;
            pending_report.payload_len = mesh_runtime.pending.payload_len;
            if (pending_report.payload_len > 0u) {
                memcpy(pending_report.payload,
                       mesh_runtime.pending.payload,
                       pending_report.payload_len);
            }
            requeue_report = true;
        }
        mesh_relay_cancel_tx(&mesh_runtime);
        (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
        LOG_INF("anchor click discovery preempted active mesh TX");
    }

    k_msgq_purge(&mesh_rx_msgq);
    if (requeue_report) {
        if (k_msgq_put(&report_tx_msgq, &pending_report, K_NO_WAIT) != 0) {
            LOG_WRN("preempted click report could not be requeued");
        }
    }
}

static void mesh_schedule_tx_timeout(void)
{
    uint32_t now = k_uptime_get_32();
    uint32_t deadline;
    uint32_t delay_ms;

    if (!mesh_relay_tx_active(&mesh_runtime) && !mesh_ch9_tx_pending.active) {
        (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
        return;
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
    delay_ms = uptime_ms_until_deadline(now, deadline);
    (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, delay_ms);
}

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

static void mesh_schedule_route_waiting_retry_after(const char *reason, uint32_t delay_ms)
{
    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, delay_ms);
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
    if (stage1_anchor_focused_rx_logs_enabled()) {
        return false;
    }
    return DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_GATEWAY;
}

static int mesh_reschedule_delayable(struct k_work_delayable *work, uint32_t delay_ms)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    return k_work_reschedule_for_queue(&mesh_route_work_q, work, K_MSEC(delay_ms));
#else
    return k_work_reschedule(work, K_MSEC(delay_ms));
#endif
}

static int mesh_submit_work(struct k_work *work)
{
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    return k_work_submit_to_queue(&mesh_route_work_q, work);
#else
    return k_work_submit(work);
#endif
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
             mesh_runtime.pending.state == MESH_RELAY_TX_WAIT_GATEWAY_ACK &&
             mesh_runtime.pending.radio_channel == UWB_CHANNEL_MESH_PAYLOAD);
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
    uint64_t peer_id;
    struct mesh_event_plan plan = {0};
    int ret;

    if (selected_plan == NULL || selected_peer == NULL ||
        !mesh_ch9_ack_batch.valid ||
        mesh_ch9_ack_batch.count == 0u) {
        return false;
    }

    peer_id = mesh_ch9_ack_batch.template_ack.next_hop_id;
    (void)mesh_advance_channel9_timing_past(peer_id, now_ms, "ack-tx-select");
    (void)mesh_expire_channel9_timings(now_ms, "ack-tx-select");
    if (!mesh_ch9_ack_batch.valid ||
        mesh_ch9_ack_batch.count == 0u ||
        mesh_ch9_ack_batch.template_ack.next_hop_id != peer_id) {
        return false;
    }
    mesh_fill_channel5_requirements(&requirements);
	    ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
	                                               peer_id,
	                                               &requirements,
	                                               now_ms,
	                                               &plan);
	    if (ret != PROTO_OK) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note(ret == PROTO_ERR_STALE ?
	                              "DBG_CH9_ACK_TX_STALE\n" :
	                              "DBG_CH9_ACK_TX_WAIT\n");
	            status_debug_printf("DBG_CH9_ACK_WAIT ret=%d act=%u now=%u start=%u n=%u\n",
	                                ret,
	                                (unsigned int)plan.action,
	                                now_ms,
	                                plan.start_ms,
	                                mesh_ch9_ack_batch.count);
	        }
	        mesh_debug_channel5_preemption("ack-tx",
	                                       "ch9-ack-batch-slot",
	                                       peer_id,
                                       &requirements,
                                       &plan,
                                       now_ms);
        return false;
    }

    mesh_event_note_plan_action(&mesh_event_stats, plan.action);
	    mesh_debug_channel5_preemption("ack-tx",
	                                   "ch9-ack-batch-slot",
	                                   peer_id,
	                                   &requirements,
	                                   &plan,
	                                   now_ms);
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        uint32_t event_counter = 0u;

	        (void)mesh_debug_channel9_state(peer_id, &event_counter, NULL);
	        status_debug_printf("DBG_CH9_ACK_SLOT cnt=%u now=%u start=%u end=%u n=%u\n",
	                            event_counter,
	                            now_ms,
	                            plan.start_ms,
	                            plan.end_ms,
	                            mesh_ch9_ack_batch.count);
	    }
	    *selected_plan = plan;
	    *selected_peer = peer_id;
	    return true;
}

static bool mesh_channel9_ack_pending_for_peer(uint64_t peer_id)
{
    return mesh_ch9_ack_batch.valid &&
           mesh_ch9_ack_batch.count > 0u &&
           mesh_ch9_ack_batch.template_ack.next_hop_id == peer_id;
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

static uint32_t mesh_next_channel9_rx_delay_ms(uint32_t now_ms)
{
    uint32_t delay_ms = mesh_uwb_rx_idle_delay_ms();

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
                                                      mesh_event_guard_start_ms(&timing));
        if (candidate_delay_ms < delay_ms) {
            delay_ms = candidate_delay_ms;
        }
    }
    return delay_ms;
}

static uint32_t mesh_active_channel9_ch5_gap_window_ms(uint32_t now_ms)
{
    uint32_t delay_ms;
    uint32_t available_ms;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        mesh_channel9_connection_count() == 0u ||
        !mesh_gateway_route_test_ch5_scan_has_capacity()) {
        return 0u;
    }

    delay_ms = mesh_next_channel9_rx_delay_ms(now_ms);
    if (delay_ms == 0u) {
        return 0u;
    }

    if (delay_ms <= MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS) {
        return 0u;
    }
    available_ms = delay_ms - MESH_ROUTE_TEST_CH5_GAP_RETUNE_MARGIN_MS;
    if (available_ms < MESH_ROUTE_TEST_CH5_GAP_MIN_SCAN_MS) {
        return 0u;
    }

    if (DEVICE_ROLE == ROLE_GATEWAY) {
        return available_ms;
    }
    return MIN(available_ms, MESH_ROUTE_TEST_CH5_GAP_SCAN_MS);
}

static void mesh_schedule_uwb_rx(uint32_t delay_ms)
{
    if (!mesh_role_uses_uwb_rx()) {
        return;
    }

    mesh_uwb_rx_active = true;
    (void)mesh_reschedule_delayable(&mesh_uwb_rx_work, delay_ms);
}

void mesh_stop_role_scan(void)
{
    if (mesh_uwb_rx_active) {
        (void)mesh_cancel_delayable(&mesh_uwb_rx_work);
        mesh_uwb_rx_active = false;
    }
}

void mesh_restart_role_scan(void)
{
    int ret;

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
    ret = radio_guard_uwb_start("mesh channel9 batch TX");
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

    (void)dwm3000_driver_standby();
    mesh_report_note_anchor_uwb_awake_since(ctx->uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();
    mesh_restart_role_scan();
    ctx->active = false;
    ctx->uwb_window_start_ms = -1;
}

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
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        uint32_t send_done_ms = k_uptime_get_32();

        status_debug_printf("DBG_CH9_TX_DONE now=%u dur=%u len=%u msg=%02x\n",
                            send_done_ms,
                            uptime_ms_until_deadline(send_begin_ms, send_done_ms),
                            (unsigned int)frame_len,
                            tx->packet.msg_type);
    }

    status_debug_tx_mesh_frame_sent_pulse();
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
    if (frame_len_out != NULL) {
        *frame_len_out = frame_len;
    }
    return 0;
}

int mesh_send_outbound(const struct mesh_outbound *out, const char *reason)
{
    struct mesh_outbound *tx = &mesh_send_scratch_tx;
    uint8_t *frame = mesh_send_scratch_frame;
    size_t frame_len = 0u;
    int64_t uwb_window_start_ms = -1;
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
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
        ret = -EINVAL;
        goto out_unlock;
    }

    mesh_stop_role_scan();
    ret = radio_guard_uwb_start("mesh UWB TX");
    if (ret < 0) {
        mesh_restart_role_scan();
        goto out_unlock;
    }
    uwb_window_start_ms = k_uptime_get();
    ret = tx->radio_channel == UWB_CHANNEL_MESH_PAYLOAD ?
          dwm3000_driver_configure_mesh_payload_mode() :
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
        ret = dwm3000_driver_send_frame(frame, frame_len, UWB_MESH_TX_TIMEOUT_MS);
    }
    (void)dwm3000_driver_standby();
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

    status_debug_tx_mesh_frame_sent_pulse();
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
    ret = 0;

out_unlock:
    k_mutex_unlock(&mesh_send_scratch_lock);
    return ret;
}

static int mesh_send_route_reply_train(const struct mesh_outbound *route_reply)
{
    uint8_t repeat_count = 1u;
    uint8_t sent_count = 0u;
    int last_ret = -EINVAL;

    if (route_reply == NULL) {
        return -EINVAL;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        DEVICE_ROLE == ROLE_GATEWAY &&
        mesh_id_is_unicast(route_reply->next_hop_id)) {
        repeat_count = MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_COUNT;
        LOG_INF("mesh route-reply turnaround delay: next=0x%016llx delay_ms=%u repeats=%u gap_ms=%u",
                (unsigned long long)route_reply->next_hop_id,
                MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS,
                repeat_count,
                MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS);
        k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_DELAY_MS);
    }

    for (uint8_t i = 0u; i < repeat_count; i++) {
        if (i > 0u) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_REPEAT_GAP_MS);
        }

        last_ret = mesh_send_outbound(route_reply,
                                      i == 0u ? "route-reply" :
                                                "route-reply-repeat");
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            DEVICE_ROLE == ROLE_GATEWAY) {
            status_debug_printf("DBG_ROUTE_REPLY_TX_REPEAT idx=%u ret=%d sent=%u\n",
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

static int mesh_ack_payload_contains_packet(const struct proto_packet *ack_packet,
                                            const uint8_t *payload,
                                            size_t payload_len,
                                            uint32_t requested_session_id,
                                            uint16_t requested_seq,
                                            bool *contains)
{
    const uint8_t *seq_value = NULL;
    const uint8_t *session_value = NULL;
    uint8_t seq_value_len = 0u;
    uint8_t session_value_len = 0u;
    uint8_t seq_count;
    int ret;
    int session_ret;

    if (contains == NULL) {
        return PROTO_ERR_ARG;
    }
    *contains = false;

    ret = tlv_find(payload, payload_len, TLV_MESH_ACK_SEQ_LIST,
                   &seq_value, &seq_value_len);
    if (ret != PROTO_OK && ret != PROTO_ERR_NOT_FOUND) {
        return ret;
    }
    session_ret = tlv_find(payload, payload_len, TLV_MESH_ACK_SESSION_LIST,
                           &session_value, &session_value_len);
    if (session_ret != PROTO_OK && session_ret != PROTO_ERR_NOT_FOUND) {
        return session_ret;
    }

    if (ret == PROTO_OK) {
        if ((seq_value_len % sizeof(uint16_t)) != 0u) {
            return PROTO_ERR_MALFORMED;
        }
        seq_count = seq_value_len / sizeof(uint16_t);
        if (session_ret == PROTO_OK &&
            session_value_len != seq_count * sizeof(uint32_t)) {
            return PROTO_ERR_MALFORMED;
        }

        for (uint8_t i = 0u; i < seq_count; i++) {
            uint16_t seq = proto_get_u16_le(&seq_value[i * sizeof(uint16_t)]);

            if (seq != requested_seq) {
                continue;
            }
            if (session_ret == PROTO_OK) {
                uint32_t session_id =
                    proto_get_u32_le(&session_value[i * sizeof(uint32_t)]);

                if (session_id != requested_session_id) {
                    continue;
                }
            }
            *contains = true;
            return PROTO_OK;
        }
        return PROTO_OK;
    }

    ret = tlv_find(payload, payload_len, TLV_REQUESTED_MSG_SEQ,
                   &seq_value, &seq_value_len);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return PROTO_OK;
    }
    if (ret != PROTO_OK) {
        return ret;
    }
    if (seq_value_len != sizeof(uint16_t)) {
        return PROTO_ERR_MALFORMED;
    }
    if (proto_get_u16_le(seq_value) == requested_seq &&
        ack_packet != NULL &&
        ack_packet->session_id == requested_session_id) {
        *contains = true;
    }
    return PROTO_OK;
}

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

static bool mesh_ch9_ack_batch_matches(const struct mesh_ch9_ack_batch *batch,
                                       const struct mesh_outbound *ack)
{
    return batch != NULL && ack != NULL && batch->valid &&
           batch->template_ack.packet.msg_type == ack->packet.msg_type &&
           batch->template_ack.packet.dst_id == ack->packet.dst_id &&
           batch->template_ack.next_hop_id == ack->next_hop_id;
}

static void mesh_ch9_ack_batch_reset_from(const struct mesh_outbound *ack)
{
    memset(&mesh_ch9_ack_batch, 0, sizeof(mesh_ch9_ack_batch));
    if (ack != NULL) {
        mesh_ch9_ack_batch.template_ack = *ack;
        mesh_ch9_ack_batch.template_ack.radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
        mesh_ch9_ack_batch.valid = true;
    }
}

static void mesh_ch9_ack_batch_queue(const struct mesh_outbound *ack,
                                     const struct mesh_rx_pending *rx)
{
    uint32_t packet_id = 0u;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        ack == NULL ||
        rx == NULL ||
        rx->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
        (ack->packet.msg_type != MSG_GATEWAY_ACK &&
         ack->packet.msg_type != MSG_MESH_HOP_ACK)) {
        return;
    }

	    if (!mesh_ch9_ack_batch_matches(&mesh_ch9_ack_batch, ack)) {
	        if (mesh_ch9_ack_batch.valid) {
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                status_debug_note("DBG_CH9_ACK_QUEUE_REPLACE\n");
	            }
        }
        mesh_ch9_ack_batch_reset_from(ack);
    }

	    for (uint8_t i = 0u; i < mesh_ch9_ack_batch.count; i++) {
	        if (mesh_ch9_ack_batch.entries[i].session_id == rx->packet.session_id &&
	            mesh_ch9_ack_batch.entries[i].seq == rx->packet.seq) {
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                status_debug_note("DBG_CH9_ACK_QUEUE_DUP\n");
	            }
	            return;
	        }
	    }

	    if (mesh_ch9_ack_batch.count >= MESH_CH9_ACK_BATCH_MAX) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_ACK_QUEUE_FULL\n");
	            status_debug_printf("DBG_CH9_ACK_QUEUE_FULL count=%u seq=%u\n",
	                                mesh_ch9_ack_batch.count,
	                                rx->packet.seq);
	        }
        LOG_WRN("mesh channel-9 ACK batch full: dst=0x%016llx count=%u dropped_seq=%u",
                (unsigned long long)ack->packet.dst_id,
                mesh_ch9_ack_batch.count,
                rx->packet.seq);
        return;
    }

    struct mesh_ch9_ack_entry *entry =
        &mesh_ch9_ack_batch.entries[mesh_ch9_ack_batch.count++];
    entry->seq = rx->packet.seq;
    entry->session_id = rx->packet.session_id;
	    entry->has_packet_id = mesh_payload_find_u32(rx->payload,
	                                                 rx->payload_len,
	                                                 TLV_MESH_TEST_PACKET_ID,
	                                                 &packet_id);
	    entry->packet_id = entry->has_packet_id ? packet_id : 0u;
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        status_debug_note("DBG_CH9_ACK_QUEUE_ADD\n");
	    }
    mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
}

static int mesh_ch9_ack_batch_build(struct mesh_outbound *out)
{
    uint8_t seq_list[MESH_CH9_ACK_BATCH_MAX * sizeof(uint16_t)];
    uint8_t session_list[MESH_CH9_ACK_BATCH_MAX * sizeof(uint32_t)];
    uint8_t packet_id_list[MESH_CH9_ACK_BATCH_MAX * sizeof(uint32_t)];
    size_t payload_len = 0u;
    int ret;

    if (out == NULL || !mesh_ch9_ack_batch.valid || mesh_ch9_ack_batch.count == 0u) {
        return -ENOENT;
    }

    *out = mesh_ch9_ack_batch.template_ack;
    out->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;

    for (uint8_t i = 0u; i < mesh_ch9_ack_batch.count; i++) {
        proto_put_u16_le(&seq_list[i * sizeof(uint16_t)], mesh_ch9_ack_batch.entries[i].seq);
        proto_put_u32_le(&session_list[i * sizeof(uint32_t)],
                         mesh_ch9_ack_batch.entries[i].session_id);
        proto_put_u32_le(&packet_id_list[i * sizeof(uint32_t)],
                         mesh_ch9_ack_batch.entries[i].packet_id);
    }

    ret = mesh_append_requested_seq(out->payload,
                                    sizeof(out->payload),
                                    &payload_len,
                                    mesh_ch9_ack_batch.entries[0].seq);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_bytes(out->payload,
                           sizeof(out->payload),
                           &payload_len,
                           TLV_MESH_ACK_SESSION_LIST,
                           session_list,
                           (uint8_t)(mesh_ch9_ack_batch.count * sizeof(uint32_t)));
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_bytes(out->payload,
                           sizeof(out->payload),
                           &payload_len,
                           TLV_MESH_ACK_SEQ_LIST,
                           seq_list,
                           (uint8_t)(mesh_ch9_ack_batch.count * sizeof(uint16_t)));
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }
    ret = tlv_append_bytes(out->payload,
                           sizeof(out->payload),
                           &payload_len,
                           TLV_MESH_ACK_PACKET_ID_LIST,
                           packet_id_list,
                           (uint8_t)(mesh_ch9_ack_batch.count * sizeof(uint32_t)));
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    out->payload_len = (uint16_t)payload_len;
    out->packet.payload_len = (uint16_t)payload_len;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_ACK_BUILD n=%u first_session=%u first_seq=%u first_pid=%u last_pid=%u\n",
                            mesh_ch9_ack_batch.count,
                            mesh_ch9_ack_batch.entries[0].session_id,
                            mesh_ch9_ack_batch.entries[0].seq,
                            mesh_ch9_ack_batch.entries[0].packet_id,
                            mesh_ch9_ack_batch.entries[mesh_ch9_ack_batch.count - 1u].packet_id);
    }
    return 0;
}

static int mesh_send_pending_ch9_ack_batch(const struct mesh_event_plan *plan,
                                           uint64_t peer_id,
                                           const char *reason)
{
    struct mesh_outbound ack = {0};
    uint8_t count;
    int ret;

    if (!mesh_ch9_ack_batch.valid ||
        mesh_ch9_ack_batch.count == 0u ||
        mesh_ch9_ack_batch.template_ack.next_hop_id != peer_id) {
        return -ENOENT;
    }

    count = mesh_ch9_ack_batch.count;
    ret = mesh_ch9_ack_batch_build(&ack);
    if (ret < 0) {
        return ret;
    }
	    if (plan != NULL) {
	        uint32_t required_ms = 0u;
	        uint32_t now_ms = k_uptime_get_32();

	        if (!mesh_ch9_tx_fits_plan(&ack, plan, now_ms, &required_ms)) {
	            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	                status_debug_note("DBG_CH9_ACK_BATCH_SLOT_FULL\n");
	                status_debug_printf("DBG_CH9_ACK_SLOT_FULL now=%u end=%u req=%u n=%u\n",
	                                    now_ms,
	                                    plan->end_ms,
	                                    required_ms,
	                                    count);
	            }
            return -EBUSY;
        }
    }

    if (plan != NULL) {
        ack.earliest_tx_ms = mesh_ch9_slot_send_start_ms(&ack,
                                                         plan,
                                                         k_uptime_get_32());
    }
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        status_debug_note("DBG_CH9_ACK_BATCH_TRY\n");
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
        memset(&mesh_ch9_ack_batch, 0, sizeof(mesh_ch9_ack_batch));
	    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        status_debug_note("DBG_CH9_ACK_BATCH_FAIL\n");
	        status_debug_printf("DBG_CH9_ACK_SEND_FAIL ret=%d n=%u\n", ret, count);
	    }
    return ret;
}

static void mesh_ch9_tx_pending_clear(void)
{
    memset(&mesh_ch9_tx_pending, 0, sizeof(mesh_ch9_tx_pending));
}

static uint8_t mesh_ch9_tx_pending_requeue_unacked(uint32_t now_ms)
{
    struct mesh_outbound rotate;
    uint8_t queued_before = (uint8_t)k_msgq_num_used_get(&report_tx_msgq);
    uint8_t requeued = 0u;
    uint8_t dropped = 0u;

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        struct mesh_ch9_tx_pending_entry *entry = &mesh_ch9_tx_pending.entries[i];

        if (entry->acked) {
            continue;
        }

        entry->outbound.queued_at_ms = now_ms;
        if (k_msgq_put(&report_tx_msgq, &entry->outbound, K_NO_WAIT) == 0) {
            requeued++;
        } else {
            dropped++;
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
        }
    }

    for (uint8_t i = 0u; i < queued_before && requeued > 0u; i++) {
        if (k_msgq_get(&report_tx_msgq, &rotate, K_NO_WAIT) != 0) {
            break;
        }
        if (k_msgq_put(&report_tx_msgq, &rotate, K_NO_WAIT) != 0) {
            dropped++;
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
            break;
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_REQUEUE_PARTIAL requeued=%u dropped=%u q_before=%u q_after=%u\n",
                            requeued,
                            dropped,
                            queued_before,
                            k_msgq_num_used_get(&report_tx_msgq));
    }
    return requeued;
}

static bool mesh_ch9_tx_pending_can_start(void)
{
    return !mesh_ch9_tx_pending.active || mesh_ch9_tx_pending.count == 0u;
}

static bool mesh_ch9_tx_pending_add(const struct mesh_outbound *sent,
                                    uint32_t deadline_ms)
{
    struct mesh_ch9_tx_pending_entry *entry;
    uint32_t packet_id = 0u;

    if (sent == NULL ||
        (sent->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u ||
        sent->packet.src_id != DEVICE_ID ||
        sent->packet.dst_id == DEVICE_ID ||
        sent->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
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
	        sent == NULL ||
	        sent->radio_channel != UWB_CHANNEL_MESH_PAYLOAD ||
	        (sent->packet.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u) {
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
                                           size_t payload_len)
{
    uint64_t ack_peer_id = 0u;
    uint8_t acked_now = 0u;

	    if (packet == NULL) {
	        return false;
	    }
	    if (packet->msg_type != MSG_GATEWAY_ACK && packet->msg_type != MSG_MESH_HOP_ACK) {
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
	    if (packet->dst_id != DEVICE_ID) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_TX_ACK_DST_MISMATCH\n");
	            status_debug_printf("DBG_CH9_TX_ACK_DST dst=0x%llx expect=0x%llx\n",
	                                (unsigned long long)packet->dst_id,
	                                (unsigned long long)DEVICE_ID);
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
        bool contains = false;
        int ret;

        if (mesh_ch9_tx_pending.entries[i].acked) {
            continue;
        }
        ret = mesh_ack_payload_contains_packet(packet,
                                               payload,
                                               payload_len,
                                               mesh_ch9_tx_pending.entries[i].outbound.packet.session_id,
                                               mesh_ch9_tx_pending.entries[i].outbound.packet.seq,
                                               &contains);
        if (ret != PROTO_OK) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_ACK_MALFORMED\n");
                status_debug_printf("DBG_CH9_TX_ACK_BAD_PAYLOAD ret=%d seq=%u\n",
                                    ret,
                                    mesh_ch9_tx_pending.entries[i].outbound.packet.seq);
            }
            return false;
        }
        if (contains) {
            mesh_ch9_tx_pending.entries[i].acked = true;
            acked_now++;
        }
    }

	    if (acked_now == 0u) {
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

    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        if (!mesh_ch9_tx_pending.entries[i].acked) {
            uint8_t pending_count = mesh_ch9_tx_pending.count;
            uint8_t requeued;

            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_ACK_PARTIAL\n");
                status_debug_printf("DBG_CH9_TX_ACK_PARTIAL acked=%u n=%u\n",
                                    acked_now,
                                    pending_count);
            }
            requeued = mesh_ch9_tx_pending_requeue_unacked(k_uptime_get_32());
            mesh_ch9_tx_pending_clear();
            mesh_schedule_tx_timeout();
            if (requeued > 0u || report_tx_queue_used() > 0u) {
                report_tx_schedule(0u);
            }
            return true;
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_ACK_COMPLETE acked=%u n=%u\n",
                            acked_now,
                            mesh_ch9_tx_pending.count);
        if (report_tx_queue_used() > 0u) {
            status_debug_printf("DBG_CH9_TX_ACK_QUEUE_PARTIAL acked=%u queued=%u\n",
                                acked_now,
                                report_tx_queue_used());
        }
    }
    ack_peer_id = mesh_ch9_tx_pending.next_hop_id;
    mesh_ch9_tx_pending_clear();
    mesh_schedule_tx_timeout();
    report_tx_schedule(0u);
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
        report_tx_queue_used() == 0u &&
        !mesh_route_waiting_tx_valid &&
        !mesh_ch9_ack_batch.valid) {
        mesh_close_channel9_connection(ack_peer_id, "ch9-idle-ack-complete");
    }
    return true;
}

static int mesh_send_route_wake_train(uint64_t target_id, const char *reason)
{
    struct uwb_clicker_session session;
    struct uwb_clicker_config config;
    const struct app_clicker_wake_train_config wake_train_config = {
        .wake_adv_ms = WAKE_ADV_MS,
        .post_wake_claimed_duration_ms = UWB_POST_WAKE_CLAIMED_DURATION_MS,
        .control_tx_timeout_ms = UWB_CONTROL_TX_TIMEOUT_MS,
    };
    uint32_t event_seq;
    int ret;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        DEVICE_ROLE == ROLE_GATEWAY) {
        return 0;
    }
    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && anchor_uwb_window_active()) {
        return -EBUSY;
    }

    event_seq = nonzero_uptime_session_id();
    memset(&config, 0, sizeof(config));
    config.network_id = NETWORK_ID;
    config.clicker_id = DEVICE_ID;
    config.click_event_id = event_seq;
    config.nonce = clicker_nonce(event_seq);
    config.min_anchor_count = 1u;
    config.max_anchor_count = 1u;
    config.max_attempts = 1u;
    config.samples_per_anchor = 1u;
    config.wake_channel = UWB_CHANNEL_WAKE_CONTACT;
    config.ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
    config.flags = FLAG_DIAGNOSTIC | FLAG_RANGE_ONLY;

    ret = uwb_clicker_session_start(&session, &config);
    if (ret != PROTO_OK) {
        LOG_WRN("mesh route wake-train session rejected: target=0x%016llx ret=%d reason=%s",
                (unsigned long long)target_id,
                ret,
                reason == NULL ? "route" : reason);
        return -EINVAL;
    }

    mesh_stop_role_scan();
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=start target=0x%016llx event_seq=%u reason=%s",
                         (unsigned long long)target_id,
                         event_seq,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train start: target=0x%016llx event_seq=%u reason=%s",
            (unsigned long long)target_id,
            event_seq,
            reason == NULL ? "route" : reason);
    ret = app_clicker_send_wake_claim_train(&session,
                                            clicker_priority_id(event_seq, session.attempt_index),
                                            &wake_train_config);
    high_debug_log_event("MESH_CH5_WAKE_TX",
                         "phase=done target=0x%016llx event_seq=%u ret=%d wake_claims=%u reason=%s",
                         (unsigned long long)target_id,
                         event_seq,
                         ret,
                         session.diagnostics.wake_claim_tx_count,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route channel-5 wake train complete: target=0x%016llx ret=%d wake_claims=%u reason=%s",
            (unsigned long long)target_id,
            ret,
            session.diagnostics.wake_claim_tx_count,
            reason == NULL ? "route" : reason);
    return ret;
}

static int mesh_listen_for_route_reply(uint64_t target_id,
                                       const char *reason,
                                       bool *route_reply_captured)
{
    struct mesh_reply_capture captures[MESH_ROUTE_TEST_REPLY_CAPTURE_MAX];
    size_t capture_count = 0u;
    uint8_t frame[UWB_MESH_MAX_FRAME_LEN];
    int64_t uwb_window_start_ms = -1;
    uint32_t deadline_ms;
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

    mesh_stop_role_scan();
    ret = radio_guard_uwb_start("mesh route reply RX");
    if (ret < 0) {
        mesh_restart_role_scan();
        return ret;
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=start target=0x%016llx window_ms=%u reason=%s",
                         (unsigned long long)target_id,
                         MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route reply listen start: target=0x%016llx window_ms=%u reason=%s",
            (unsigned long long)target_id,
            MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS,
            reason == NULL ? "route" : reason);

    uwb_window_start_ms = k_uptime_get();
    ret = dwm3000_driver_configure_wake_mode();
    if (ret == 0) {
        deadline_ms = k_uptime_get_32() + MESH_ROUTE_TEST_REPLY_RX_WINDOW_MS;
        while (!uptime_deadline_reached(k_uptime_get_32(), deadline_ms) &&
               capture_count < ARRAY_SIZE(captures)) {
            struct mesh_frame_parse_context parsed = {0};
            enum dwm3000_rx_failure rx_failure = DWM3000_RX_FAILURE_NONE;
            uint32_t now_ms = k_uptime_get_32();
            uint32_t remaining_ms = uptime_ms_until_deadline(now_ms, deadline_ms);
            size_t frame_len = 0u;
            uint8_t quality = 0u;

            if (remaining_ms == 0u) {
                break;
            }

            ret = dwm3000_driver_receive_frame_continuous(remaining_ms,
                                                          frame,
                                                          sizeof(frame),
                                                          &frame_len,
                                                          &quality,
                                                          NULL,
                                                          &rx_failure);
            last_ret = ret;
            if (ret == -ETIMEDOUT) {
                break;
            }
            if (ret < 0) {
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
            if (mesh_handle_channel5_wake_claim(frame, frame_len, quality)) {
                continue;
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
            } else if (parsed.packet.msg_type == MSG_MESH_EVENT_PROPOSE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_ACCEPT ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_UPDATE ||
                       parsed.packet.msg_type == MSG_MESH_EVENT_END) {
                status_debug_note("DBG_EVENT_CTRL_RX\n");
            }

            if (!((parsed.packet.msg_type == MSG_ROUTE_REPLY &&
                   parsed.packet.src_id == target_id &&
                   parsed.packet.dst_id == DEVICE_ID) ||
                  (mesh_packet_is_event_control_type(parsed.packet.msg_type) &&
                   parsed.packet.dst_id == DEVICE_ID))) {
                LOG_INF("mesh route reply listen ignored unrelated mesh frame: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx seq=%u quality=%u",
                        parsed.packet.msg_type,
                        (unsigned long long)parsed.packet.src_id,
                        (unsigned long long)parsed.packet.dst_id,
                        (unsigned long long)parsed.previous_hop_id,
                        parsed.packet.seq,
                        quality);
                continue;
            }

            memcpy(captures[capture_count].frame, frame, frame_len);
            captures[capture_count].frame_len = frame_len;
            captures[capture_count].quality = quality;
            captures[capture_count].received_at_ms = k_uptime_get_32();
            capture_count++;
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
            if (parsed.packet.msg_type == MSG_ROUTE_REPLY) {
                if (route_reply_captured != NULL) {
                    *route_reply_captured = true;
                }
                high_debug_log_event("MESH_ROUTE_REPLY_RX",
                                     "phase=route-reply-captured target=0x%016llx seq=%u captured=%u reason=%s",
                                     (unsigned long long)target_id,
                                     parsed.packet.seq,
                                     (unsigned int)capture_count,
                                     reason == NULL ? "route" : reason);
                LOG_INF("mesh route reply listen exiting after route reply: target=0x%016llx seq=%u captured=%u",
                        (unsigned long long)target_id,
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
    mesh_restart_role_scan();

    for (size_t i = 0u; i < capture_count; i++) {
        bool valid_mesh_frame = false;
        uint64_t previous_hop_id = 0u;

        (void)mesh_queue_from_frame_at(captures[i].frame,
                                       captures[i].frame_len,
                                       captures[i].quality,
                                       UWB_CHANNEL_WAKE_CONTACT,
                                       captures[i].received_at_ms,
                                       &valid_mesh_frame,
                                       &previous_hop_id);
        ARG_UNUSED(valid_mesh_frame);
        ARG_UNUSED(previous_hop_id);
    }

    high_debug_log_event("MESH_ROUTE_REPLY_RX",
                         "phase=done target=0x%016llx captured=%u last_ret=%d reason=%s",
                         (unsigned long long)target_id,
                         (unsigned int)capture_count,
                         last_ret,
                         reason == NULL ? "route" : reason);
    LOG_INF("mesh route reply listen done: target=0x%016llx captured=%u last_ret=%d reason=%s",
            (unsigned long long)target_id,
            (unsigned int)capture_count,
            last_ret,
            reason == NULL ? "route" : reason);
    return capture_count > 0u ? 0 : last_ret;
}

int mesh_request_route(uint64_t target_id, const char *reason)
{
    struct mesh_outbound route_req;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (!mesh_id_is_unicast(target_id) || target_id == DEVICE_ID) {
        return -EINVAL;
    }

    ret = mesh_relay_prepare_route_request(&mesh_runtime,
                                           target_id,
                                           now_ms,
                                           sys_rand32_get(),
                                           &route_req);
    if (ret != PROTO_OK) {
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

    LOG_INF("mesh route discovery request: target=0x%016llx attempt=%u next_ms=%u reason=%s",
            (unsigned long long)target_id,
            mesh_runtime.route_discovery.attempts,
            mesh_runtime.route_discovery.next_request_ms,
            reason);
    ret = mesh_send_route_wake_train(target_id, reason);
    if (ret < 0) {
        LOG_WRN("mesh route discovery wake train failed: target=0x%016llx attempt=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                ret,
                reason);
        mesh_restart_role_scan();
        mesh_schedule_route_waiting_retry(reason);
        return ret;
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
    ret = mesh_send_outbound(&route_req, "route-request");
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
        bool route_reply_captured = false;
        int listen_ret = mesh_listen_for_route_reply(target_id,
                                                     reason,
                                                     &route_reply_captured);

        if (listen_ret < 0 && listen_ret != -ETIMEDOUT) {
            LOG_WRN("mesh route reply listen failed after request TX: target=0x%016llx attempt=%u ret=%d reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    listen_ret,
                    reason == NULL ? "route" : reason);
        }
        if (route_reply_captured) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_REPLY_HANDOFF\n");
            }
            mesh_route_reply_handoff_begin();
            if (!mesh_ch9_tx_pending.active &&
                !mesh_relay_tx_active(&mesh_runtime)) {
                (void)mesh_cancel_delayable(&mesh_tx_timeout_work);
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_ROUTE_REPLY_CANCEL_RETRY\n");
                }
            }
            LOG_INF("mesh route reply captured; waiting for queued route processing: target=0x%016llx attempt=%u reason=%s",
                    (unsigned long long)target_id,
                    mesh_runtime.route_discovery.attempts,
                    reason == NULL ? "route" : reason);
        } else {
            mesh_schedule_route_waiting_retry(reason);
        }
    } else {
        LOG_WRN("mesh route discovery request TX failed: target=0x%016llx attempt=%u next_ms=%u ret=%d reason=%s",
                (unsigned long long)target_id,
                mesh_runtime.route_discovery.attempts,
                mesh_runtime.route_discovery.next_request_ms,
                ret,
                reason);
        mesh_schedule_route_waiting_retry(reason);
    }
    return ret;
}

static bool mesh_packet_prefers_channel9(const struct proto_packet *packet)
{
    if (packet == NULL) {
        return false;
    }

    switch (packet->msg_type) {
    case MSG_CLICK_REPORT:
    case MSG_SELF_TEST_REPORT:
    case MSG_ANCHOR_HEARTBEAT:
    case MSG_MESH_DATA:
    case MSG_GATEWAY_ACK:
    case MSG_COMMAND:
    case MSG_COMMAND_RESULT:
    case MSG_SURVEY_REACH_REPORT:
    case MSG_SURVEY_PAIR_PREPARE:
    case MSG_SURVEY_PAIR_RESULT:
    case MSG_SURVEY_DISCOVERY_REPORT:
        return true;
    default:
        return false;
    }
}

static void mesh_fill_channel5_requirements(struct mesh_channel5_requirements *requirements)
{
    uint32_t now_ms;

    if (requirements == NULL) {
        return;
    }

    memset(requirements, 0, sizeof(*requirements));
    requirements->retune_guard_ms = MESH_EVENT_DEFAULT_GUARD_MS;
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

static int mesh_send_event_control(uint64_t peer_id,
                                   uint8_t msg_type,
                                   const struct mesh_event_timing *accepted_timing,
                                   bool install_local,
                                   const char *reason)
{
    struct mesh_event_timing timing = {0};
    struct mesh_outbound outbound = {0};
    size_t payload_len = 0u;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t timing_reference_ms = mesh_event_control_tx_reference_ms(msg_type, now_ms);
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
    ret = mesh_send_outbound(&outbound, reason == NULL ? "mesh-event-control" : reason);
    if (ret < 0) {
        return ret;
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
                                                      mesh_event_guard_start_ms(&local_timing)));
    }
    return 0;
}

static int mesh_propose_event_after_channel5_contact(uint64_t peer_id, const char *reason)
{
    bool require_accept = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                          DEVICE_ROLE != ROLE_GATEWAY;
    int ret;

    ret = mesh_send_event_control(peer_id,
                                  MSG_MESH_EVENT_PROPOSE,
                                  NULL,
                                  true,
                                  reason);
    if (ret < 0) {
        LOG_WRN("mesh channel-9 event proposal failed: peer=0x%016llx ret=%d reason=%s",
                (unsigned long long)peer_id,
                ret,
                reason == NULL ? "event-propose" : reason);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_PROPOSE_FAIL\n");
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
        ret = mesh_listen_for_route_reply(peer_id, "event-accept", NULL);
        if (ret < 0) {
            (void)mesh_ch9_ack_batch_clear_for_peer(peer_id, "event-accept-timeout");
            mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_EVENT_ACCEPT_TIMEOUT\n");
            }
            LOG_WRN("mesh channel-9 event ACCEPT not received: peer=0x%016llx ret=%d",
                    (unsigned long long)peer_id,
                    ret);
            return ret;
        }
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_EVENT_ACCEPT_RX\n");
        }
    }
    return 0;
}

static bool mesh_packet_is_event_control_type(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

static bool mesh_handle_event_control(const struct proto_packet *packet,
                                      const uint8_t *payload,
                                      size_t payload_len,
                                      uint64_t previous_hop_id,
                                      uint32_t received_at_ms)
{
    struct mesh_event_timing timing = {0};
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
        bool use_active_timing = false;
        uint32_t now_ms = k_uptime_get_32();

        mesh_event_timing_set_local_first_slot_tx(&timing, false);
        use_active_timing = mesh_find_active_channel9_timing(previous_hop_id,
                                                            now_ms,
                                                            &active_timing);
        if (use_active_timing) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_printf("DBG_EVENT_PROPOSE_ACTIVE now=%u old=%u new=%u cnt=%u\n",
                                    now_ms,
                                    active_timing.next_event_time_ms,
                                    timing.next_event_time_ms,
                                    active_timing.event_counter);
            }
            timing = active_timing;
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
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
        }
        ret = mesh_send_event_control(previous_hop_id,
                                      MSG_MESH_EVENT_ACCEPT,
                                      &timing,
                                      false,
                                      "mesh-event-accept");
        if (ret < 0) {
            LOG_WRN("mesh channel-9 event ACCEPT failed: next=0x%016llx ret=%d",
                    (unsigned long long)previous_hop_id,
                    ret);
            return true;
        }
        if (use_active_timing) {
            mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                          mesh_event_guard_start_ms(&timing)));
            return true;
        }
    } else if (packet->msg_type == MSG_MESH_EVENT_ACCEPT) {
        mesh_event_timing_set_local_first_slot_tx(&timing, true);
    }

    ret = mesh_install_channel9_timing(previous_hop_id,
                                       &timing,
                                       "event-control-rx");
    if (ret != PROTO_OK) {
        LOG_WRN("mesh event control timing install failed: next=0x%016llx ret=%d",
                (unsigned long long)previous_hop_id,
                ret);
        return true;
    }

    mesh_schedule_uwb_rx(uptime_ms_until_deadline(k_uptime_get_32(),
                                                  mesh_event_guard_start_ms(&timing)));
    if (mesh_gateway_route_test_role() &&
        packet->msg_type == MSG_MESH_EVENT_PROPOSE) {
        mesh_gateway_route_test_clear_preempt(previous_hop_id,
                                              "event-accept-installed");
    }
    return true;
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
                                  reason == NULL ? "mesh-event-end" : reason);
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
    mesh_relay_clear_channel9_timing(&mesh_runtime, peer_id);
    mesh_restore_anchor_low_duty_if_no_ch9("event-end-tx");
}

static bool mesh_tx_can_wait_for_route(const struct mesh_outbound *out)
{
    if (out == NULL ||
        out->packet.dst_id == MESH_BROADCAST_ID ||
        out->packet.dst_id == DEVICE_ID) {
        return false;
    }

    switch (out->packet.msg_type) {
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

    if (out == NULL || plan == NULL || out->radio_channel != UWB_CHANNEL_MESH_PAYLOAD) {
        return true;
    }

    needed_ms = mesh_ch9_estimated_tx_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    effective_send_start_ms = mesh_ch9_effective_send_start_ms(out, plan, now_ms);
    available_ms = uptime_ms_until_deadline(effective_send_start_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = uptime_ms_until_deadline(now_ms, effective_send_start_ms) +
                       needed_ms;
    }
    return available_ms >= needed_ms;
}

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

    wait_ms = uptime_ms_until_deadline(now_ms, send_start_ms);
    needed_ms = wait_ms +
                mesh_ch9_estimated_airtime_ms(mesh_outbound_encoded_frame_len(out)) +
                MESH_CH9_TX_SLOT_TRAILER_MS;
    available_ms = uptime_ms_until_deadline(now_ms, plan->end_ms);
    if (required_ms != NULL) {
        *required_ms = needed_ms;
    }
    return available_ms >= needed_ms;
}

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

    if (!mesh_tx_can_wait_for_route(out)) {
        return;
    }

    waiting = *out;
    if (waiting.queued_at_ms == 0u) {
        waiting.queued_at_ms = k_uptime_get_32();
    }
    mesh_route_waiting_tx = waiting;
    mesh_route_waiting_tx_valid = true;
}

static void mesh_drop_route_waiting_tx(const char *reason)
{
    if (!mesh_route_waiting_tx_valid) {
        return;
    }

    high_debug_log_event("MESH_ROUTE_WAIT_DROP",
                         "reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u",
                         reason == NULL ? "drop" : reason,
                         mesh_route_waiting_tx.packet.msg_type,
                         (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
                         mesh_route_waiting_tx.packet.seq,
                         mesh_runtime.route_discovery.attempts);
    LOG_WRN("mesh route waiting packet dropped: reason=%s msg=0x%02x dst=0x%016llx seq=%u attempts=%u",
            reason == NULL ? "drop" : reason,
            mesh_route_waiting_tx.packet.msg_type,
            (unsigned long long)mesh_route_waiting_tx.packet.dst_id,
            mesh_route_waiting_tx.packet.seq,
            mesh_runtime.route_discovery.attempts);
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

static void mesh_try_route_waiting_tx(void)
{
    struct mesh_outbound pending;
    uint32_t now_ms;
    int ret;

    if (!mesh_route_waiting_tx_valid ||
        (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) ||
        mesh_relay_tx_active(&mesh_runtime)) {
        return;
    }
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        (mesh_route_reply_handoff_active() ||
         k_msgq_num_used_get(&mesh_rx_msgq) > 0u)) {
        status_debug_note("DBG_ROUTE_WAIT_RX_HANDOFF\n");
        mesh_schedule_route_waiting_retry_after("route-reply-handoff",
                                                MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS);
        return;
    }

    pending = mesh_route_waiting_tx;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_id_is_unicast(mesh_route_ready_event_peer_id)) {
        uint64_t peer_id = mesh_route_ready_event_peer_id;

        if (k_msgq_num_used_get(&mesh_rx_msgq) > 0u) {
            status_debug_note("DBG_ROUTE_READY_PROPOSE_WAIT_RX\n");
            mesh_schedule_route_waiting_retry("route-ready-propose-wait-rx");
            return;
        }

        status_debug_note("DBG_ROUTE_READY_PROPOSE_DRAINED\n");
        LOG_INF("mesh route-ready deferred proposal after RX drain: next=0x%016llx delay_ms=%u",
                (unsigned long long)peer_id,
                MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
        mesh_route_ready_event_peer_id = 0u;
        k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
        if (mesh_propose_event_after_channel5_contact(peer_id,
                                                      "route-ready-drained-event-propose") < 0) {
            mesh_schedule_route_waiting_retry("route-ready-event-accept-wait");
            return;
        }
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(&pending, now_ms)) {
        mesh_schedule_route_waiting_retry("route-waiting-not-ready");
        return;
    }
    ret = mesh_start_tracked_tx(&pending, "route-discovered-packet");
    if (ret == 0) {
        mesh_route_waiting_tx_valid = false;
    } else if (ret == -EHOSTUNREACH) {
        ret = mesh_request_route(pending.packet.dst_id, "route-waiting-packet");
        if (ret == -ETIMEDOUT) {
            mesh_drop_route_waiting_tx("route-waiting-timeout");
        }
    } else if (ret == -ETIMEDOUT) {
        mesh_drop_route_waiting_tx("route-waiting-stale");
    } else if (ret == -EBUSY) {
        mesh_schedule_route_waiting_retry_after("route-waiting-channel9-event",
                                                MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
    } else {
        mesh_schedule_route_waiting_retry_after("route-waiting-busy",
                                                REPORT_TX_RETRY_DELAY_MS);
    }
}

int mesh_start_tracked_tx(const struct mesh_outbound *out, const char *reason)
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
    int ret;

    if (out == NULL) {
        return -EINVAL;
    }

    aged_out = *out;
    now_ms = k_uptime_get_32();
    mesh_outbound_refresh_age(&aged_out, now_ms);

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
                mesh_relay_cancel_tx(&mesh_runtime);
                if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                    status_debug_note("DBG_CH9_TX_SINGLE_SLOT_FULL\n");
                    status_debug_note("DBG_CH9_TX_SINGLE_MISSED_SLOT\n");
                    status_debug_printf("DBG_CH9_TX_SINGLE_FULL now=%u end=%u req=%u seq=%u\n",
                                        fit_now_ms,
                                        plan.end_ms,
                                        required_ms,
                                        tx.packet.seq);
                }
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                tx.next_hop_id,
                                                &mesh_event_stats);
                return -EBUSY;
            }
            channel9_success_pending = true;
            channel9_report_latency_pending = aged_out.packet.msg_type == MSG_CLICK_REPORT;
            channel9_event_start_ms = plan.start_ms;
            channel9_next_hop_id = tx.next_hop_id;
            goto send_prepared;
        }
        if (ret == PROTO_ERR_BUSY) {
            mesh_debug_channel9_unavailable("tx",
                                            reason,
                                            &aged_out.packet,
                                            ret,
                                            debug_select_ret,
                                            debug_next_hop,
                                            &plan,
                                            now_ms);
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
            mesh_store_route_waiting_tx(&aged_out);
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_drop_route_waiting_tx("route-discovery-exhausted");
                return route_ret;
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

            mesh_store_route_waiting_tx(&aged_out);
            route_ret = mesh_request_route(aged_out.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_drop_route_waiting_tx("route-discovery-exhausted");
                return route_ret;
            }
        }
        return mesh_errno_from_proto(ret);
	    }
	send_prepared:
	    ret = mesh_send_outbound(&tx, reason);
	    if (ret < 0) {
	        if (channel9_success_pending && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_TX_SINGLE_SEND_FAIL\n");
	            status_debug_printf("DBG_CH9_TX_SINGLE_FAIL ret=%d seq=%u\n",
	                                ret,
	                                tx.packet.seq);
	        }
	        mesh_relay_cancel_tx(&mesh_runtime);
        if (ret == -EHOSTUNREACH || ret == -ETIMEDOUT || ret == -ENOTCONN) {
            int route_ret;

            mesh_relay_note_delivery_failure(&mesh_runtime, tx.packet.dst_id);
            mesh_store_route_waiting_tx(&tx);
            route_ret = mesh_request_route(tx.packet.dst_id, reason);
            if (route_ret == -ETIMEDOUT) {
                mesh_drop_route_waiting_tx("send-failure-route-exhausted");
                return route_ret;
            }
        }
        return ret;
    }
	    mesh_relay_note_tx_sent(&mesh_runtime, &tx, k_uptime_get_32());
	    if (channel9_success_pending) {
	        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	            status_debug_note("DBG_CH9_TX_SINGLE_SEND_OK\n");
	        }
	        mesh_note_channel9_local_tx(channel9_next_hop_id, channel9_event_start_ms);
        if (channel9_report_latency_pending) {
            mesh_event_note_report_latency(&mesh_event_stats,
                                           channel9_event_start_ms > now_ms ?
                                           channel9_event_start_ms - now_ms : 0u);
        }
        if (mesh_ch9_tx_pending_track_sent(&tx,
                                           k_uptime_get_32() + ROUTE_GATEWAY_ACK_TIMEOUT_MS)) {
            mesh_schedule_tx_timeout();
            return 0;
        }
    }
    mesh_schedule_tx_timeout();
    return 0;
}

static int mesh_try_send_report_tx_ch9_batch(void)
{
    struct mesh_outbound *first = &report_tx_batch_first;
    struct mesh_outbound *queued = &report_tx_batch_queued;
    struct mesh_outbound *tx = &report_tx_batch_tx;
    struct mesh_outbound *dropped = &report_tx_batch_dropped;
    struct mesh_ch9_slot_tx_context slot_tx = {
        .uwb_window_start_ms = -1,
    };
    struct mesh_channel5_requirements requirements;
    struct mesh_event_plan plan = {0};
    uint64_t next_hop_id = 0u;
    uint32_t now_ms;
    uint32_t deadline_ms;
    uint8_t sent_count = 0u;
    bool ack_wait_started = false;
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
	        return ret == PROTO_ERR_BUSY ? -EBUSY : -ENOTSUP;
	    }
	    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
	        uint32_t event_counter = 0u;

	        (void)mesh_debug_channel9_state(next_hop_id, &event_counter, NULL);
	        status_debug_printf("DBG_CH9_TX_BATCH_SLOT cnt=%u now=%u start=%u end=%u q=%u\n",
	                            event_counter,
	                            now_ms,
	                            plan.start_ms,
	                            plan.end_ms,
	                            k_msgq_num_used_get(&report_tx_msgq));
	    }

    deadline_ms = now_ms + ROUTE_GATEWAY_ACK_TIMEOUT_MS;
    k_mutex_lock(&mesh_send_scratch_lock, K_FOREVER);
    while (sent_count < MESH_CH9_TX_BATCH_MAX) {
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
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
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
                mesh_relay_note_channel9_missed(&mesh_runtime,
                                                next_hop_id,
                                                &mesh_event_stats);
                report_tx_schedule(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return -EALREADY;
            }
        }

        ret = mesh_send_outbound_preconfigured_ch9_locked(tx,
                                                          "queued-ch9-batch",
                                                          &sent_frame_len);
        if (ret < 0) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_CH9_TX_BATCH_SEND_FAIL\n");
	                status_debug_printf("DBG_CH9_TX_BATCH_FAIL ret=%d sent=%u seq=%u\n",
	                                    ret,
	                                    sent_count,
	                                    tx->packet.seq);
            }
            if (sent_count == 0u) {
                mesh_ch9_slot_tx_end(&slot_tx);
                k_mutex_unlock(&mesh_send_scratch_lock);
                return ret;
            }
            break;
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

	        (void)k_msgq_get(&report_tx_msgq, dropped, K_NO_WAIT);
	        mesh_relay_note_tx_sent(&mesh_runtime, tx, k_uptime_get_32());
        if (mesh_ch9_tx_pending_track_sent(tx, deadline_ms)) {
            ack_wait_started = true;
        }
        sent_count++;
    }
    mesh_ch9_slot_tx_end(&slot_tx);
    k_mutex_unlock(&mesh_send_scratch_lock);

    if (sent_count == 0u) {
        return -EBUSY;
    }

    mesh_note_channel9_local_tx(next_hop_id, plan.start_ms);
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_printf("DBG_CH9_TX_BATCH_SENT sent=%u ack=%u deadline=%u\n",
                            sent_count,
                            ack_wait_started ? 1u : 0u,
                            deadline_ms);
    }
    if (ack_wait_started) {
        mesh_schedule_tx_timeout();
    } else {
        report_tx_schedule(0u);
    }
    return 0;
}

void report_tx_schedule(uint32_t delay_ms)
{
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        (void)mesh_reschedule_delayable(&report_tx_work, delay_ms);
    }
}

uint32_t report_tx_queue_used(void)
{
    return (uint32_t)k_msgq_num_used_get(&report_tx_msgq);
}

bool mesh_report_tx_backlog_active(void)
{
    return k_msgq_num_used_get(&report_tx_msgq) > 0 || mesh_ch9_tx_pending.active;
}

bool mesh_report_ch9_ack_wait_active(void)
{
    return mesh_ch9_tx_pending.active;
}

static void report_tx_work_handler(struct k_work *work)
{
    struct mesh_outbound *outbound = &report_tx_work_outbound;
    struct mesh_outbound *dropped = &report_tx_work_dropped;
    bool anchor_busy;
    bool survey_busy;
    bool relay_tx_active;
    bool ch9_ack_wait_active;
    bool route_waiting_active;
    bool rx_queue_pending;
    bool route_handoff_active;
    uint32_t now_ms;
    int ret;

    ARG_UNUSED(work);

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }
    anchor_busy = anchor_uwb_window_active();
    survey_busy = mesh_report_anchor_survey_discovery_is_pending();
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    ch9_ack_wait_active = mesh_ch9_tx_pending.active;
    route_waiting_active = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                           mesh_route_waiting_tx_active();
    rx_queue_pending = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
                       k_msgq_num_used_get(&mesh_rx_msgq) > 0u;
    route_handoff_active = mesh_route_reply_handoff_active();
    if (anchor_busy || survey_busy || relay_tx_active || ch9_ack_wait_active ||
        route_waiting_active || rx_queue_pending || route_handoff_active) {
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
            k_msgq_num_used_get(&report_tx_msgq) > 0) {
            if (anchor_busy) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ANCHOR\n");
            } else if (rx_queue_pending) {
                status_debug_note("DBG_REPORT_WORK_BUSY_RX\n");
            } else if (route_handoff_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_HANDOFF\n");
            } else if (route_waiting_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ROUTE\n");
            } else if (ch9_ack_wait_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_ACK\n");
            } else if (relay_tx_active) {
                status_debug_note("DBG_REPORT_WORK_BUSY_TX\n");
            } else {
                status_debug_note("DBG_REPORT_WORK_BUSY_SURVEY\n");
            }
            report_tx_schedule((rx_queue_pending || route_handoff_active) ?
                               MESH_GATEWAY_ROUTE_PREEMPT_YIELD_MS :
                               MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
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

    ret = k_msgq_peek(&report_tx_msgq, outbound);
    if (ret != 0) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (!mesh_outbound_ready_for_tx(outbound, now_ms)) {
        report_tx_schedule(uptime_ms_until_deadline(now_ms, outbound->earliest_tx_ms));
        return;
    }

    ret = mesh_start_tracked_tx(outbound, "queued-click-report");
    if (ret == 0) {
        (void)k_msgq_get(&report_tx_msgq, dropped, K_NO_WAIT);
        return;
    }

    if (ret == -EHOSTUNREACH &&
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        mesh_route_waiting_tx_active()) {
        status_debug_note("DBG_REPORT_QUEUE_TO_ROUTE_WAIT\n");
        (void)k_msgq_get(&report_tx_msgq, dropped, K_NO_WAIT);
        return;
    }

    if (ret == -EHOSTUNREACH || ret == -EBUSY) {
        LOG_WRN("queued gateway-bound report waiting for mesh route/idle state: ret=%d", ret);
        report_tx_schedule(REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    (void)k_msgq_get(&report_tx_msgq, dropped, K_NO_WAIT);
    LOG_WRN("queued gateway-bound report dropped after permanent TX error: ret=%d", ret);
}

int queue_anchor_report(const struct mesh_outbound *outbound)
{
    struct mesh_outbound queued;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }
    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return 0;
    }

    queued = *outbound;
    queued.queued_at_ms = k_uptime_get_32();

    ret = k_msgq_put(&report_tx_msgq, &queued, K_NO_WAIT);
    if (ret != 0) {
        HIGH_DEBUG_COUNTER_INC(mesh_drop);
        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_REPORT_QUEUE_FULL\n");
        }
        LOG_WRN("anchor report queue full; gateway-bound report dropped");
        return -ENOSPC;
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
}

static void mesh_handle_result_actions(const struct mesh_relay_result *result,
                                       uint8_t received_radio_channel,
                                       const struct mesh_rx_pending *rx)
{
    bool forward_sent = false;

    if (result->actions & MESH_RELAY_ACTION_SEND_GATEWAY_ACK) {
        struct mesh_outbound *gateway_ack = &mesh_result_action_tx;
        struct mesh_channel5_requirements requirements;
        struct mesh_event_plan plan = {0};
        bool sent_on_current_channel9 = false;
        uint32_t now_ms;
        int ret;

        *gateway_ack = result->gateway_ack;
        if (received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD &&
            IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            mesh_ch9_ack_batch_queue(gateway_ack, rx);
            goto after_gateway_ack;
        }

        if (DEVICE_ROLE == ROLE_GATEWAY &&
            received_radio_channel == UWB_CHANNEL_MESH_PAYLOAD) {
            gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
            if (mesh_send_outbound(gateway_ack, "gateway-ack-current-channel9") == 0) {
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
                sent_on_current_channel9 = true;
            } else {
                LOG_WRN("gateway ACK current channel-9 send failed; will retry on channel-9 event: next=0x%016llx",
                        (unsigned long long)gateway_ack->next_hop_id);
                mesh_store_route_waiting_tx(gateway_ack);
                mesh_schedule_route_waiting_retry_after("gateway-ack-current-channel9",
                                                        MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
                sent_on_current_channel9 = true;
            }
        }

        if (!sent_on_current_channel9) {
            mesh_fill_channel5_requirements(&requirements);
            now_ms = k_uptime_get_32();
            (void)mesh_expire_channel9_timings(now_ms, "gateway-ack");
            ret = mesh_relay_require_channel9_tx_event(&mesh_runtime,
                                                       gateway_ack->next_hop_id,
                                                       &requirements,
                                                       now_ms,
                                                       &plan);
            if (ret != PROTO_ERR_STALE) {
                mesh_event_note_plan_action(&mesh_event_stats, plan.action);
            }
            mesh_debug_channel5_preemption("gateway-ack",
                                           "gateway-ack",
                                           gateway_ack->next_hop_id,
                                           &requirements,
                                           &plan,
                                           now_ms);
            if (ret != PROTO_OK) {
                LOG_WRN("gateway ACK falling back to channel-5 contact: next=0x%016llx ret=%d",
                        (unsigned long long)gateway_ack->next_hop_id,
                        ret);
                gateway_ack->radio_channel = UWB_CHANNEL_WAKE_CONTACT;
                if (mesh_send_outbound(gateway_ack, "gateway-ack-channel5") == 0) {
                    mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                } else {
                    mesh_store_route_waiting_tx(gateway_ack);
                    (void)mesh_request_route(gateway_ack->packet.dst_id,
                                             "gateway-ack-channel9-refresh");
                }
            } else {
                gateway_ack->radio_channel = MESH_EVENT_CHANNEL;
                if (mesh_send_outbound(gateway_ack, "gateway-ack") == 0) {
                    mesh_relay_note_tx_sent(&mesh_runtime, gateway_ack, k_uptime_get_32());
                    mesh_note_channel9_local_tx(gateway_ack->next_hop_id, plan.start_ms);
                }
            }
        }
    }
after_gateway_ack:
    if (result->actions & MESH_RELAY_ACTION_FORWARD) {
        int ret;

        if (result->forward.packet.dst_id == MESH_BROADCAST_ID) {
            ret = mesh_send_outbound(&result->forward, "broadcast-forward");
        } else {
            ret = mesh_start_tracked_tx(&result->forward, "forward");
        }
        forward_sent = ret == 0;
    }
    if ((result->actions & MESH_RELAY_ACTION_SEND_HOP_ACK) && forward_sent) {
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
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REQ) {
        (void)mesh_send_outbound(&result->route_request, "route-request-forward");
    }
    if (result->actions & MESH_RELAY_ACTION_SEND_ROUTE_REPLY) {
        if (mesh_send_route_reply_train(&result->route_reply) == 0 &&
            mesh_id_is_unicast(result->route_reply.next_hop_id)) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                LOG_INF("mesh route reply sent; waiting for route origin to propose channel-9 event: next=0x%016llx",
                        (unsigned long long)result->route_reply.next_hop_id);
            } else {
                (void)mesh_propose_event_after_channel5_contact(result->route_reply.next_hop_id,
                                                                "route-reply-event-propose");
            }
        }
    }
    if (result->actions & MESH_RELAY_ACTION_RETRANSMIT) {
        struct mesh_outbound *retransmit = &mesh_result_action_tx;
        struct mesh_event_plan plan = {0};
        bool channel9_replanned = false;
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
                LOG_INF("mesh retransmit waiting for channel-9 event: msg=0x%02x dst=0x%016llx start_ms=%u window_ms=%u",
                        retransmit->packet.msg_type,
                        (unsigned long long)retransmit->packet.dst_id,
                        plan.start_ms,
                        plan.window_ms);
                mesh_store_route_waiting_tx(retransmit);
                mesh_schedule_route_waiting_retry_after("retransmit-channel9-event",
                                                        MESH_ROUTE_CHANNEL9_WAIT_RETRY_MS);
            } else {
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
                mesh_store_route_waiting_tx(retransmit);
                (void)mesh_request_route(retransmit->packet.dst_id,
                                         "retransmit-channel9-refresh");
            }
        }
        if (ret == PROTO_OK && mesh_send_outbound(retransmit, "retransmit") == 0) {
            HIGH_DEBUG_COUNTER_INC(mesh_retry);
            mesh_relay_note_tx_sent(&mesh_runtime, retransmit, k_uptime_get_32());
            if (channel9_replanned) {
                mesh_note_channel9_local_tx(retransmit->next_hop_id, plan.start_ms);
            }
        }
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) {
        LOG_WRN("mesh route discovery needed after delivery failure");
    }
    if (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_READY) {
        const struct route_candidate *selected = route_selected(&mesh_runtime.upstream);
        bool rx_queue_pending = k_msgq_num_used_get(&mesh_rx_msgq) > 0u;

        if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
            status_debug_note("DBG_ROUTE_READY\n");
        }
        mesh_route_reply_handoff_clear("route-ready");
        LOG_INF("mesh reactive route ready");
        if (selected != NULL && !rx_queue_pending) {
            int propose_ret;

            if (mesh_route_ready_event_peer_id == selected->next_hop_id) {
                mesh_route_ready_event_peer_id = 0u;
            }
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_PROPOSE_NOW\n");
                LOG_INF("mesh route-ready to event-control gap: next=0x%016llx delay_ms=%u",
                        (unsigned long long)selected->next_hop_id,
                        MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
                k_msleep(MESH_ROUTE_TEST_ROUTE_REPLY_TO_EVENT_DELAY_MS);
            }
            propose_ret = mesh_propose_event_after_channel5_contact(selected->next_hop_id,
                                                                    "route-ready-event-propose");
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && propose_ret < 0) {
                mesh_schedule_route_waiting_retry("route-ready-event-accept-wait");
                return;
            }
        }
        if (rx_queue_pending) {
            if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
                status_debug_note("DBG_ROUTE_READY_RX_PENDING\n");
                if (selected != NULL) {
                    mesh_route_ready_event_peer_id = selected->next_hop_id;
                }
            }
            LOG_INF("mesh route ready deferred until queued RX control frames drain");
            mesh_schedule_route_waiting_retry("route-ready-rx-drain");
        } else {
            mesh_try_route_waiting_tx();
        }
    }
    if (result->actions & MESH_RELAY_ACTION_TX_GATEWAY_CONFIRMED) {
        HIGH_DEBUG_COUNTER_INC(mesh_ack);
        LOG_INF("mesh pending TX gateway acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_TX_HOP_PROGRESS) {
        LOG_INF("mesh pending TX hop progress acknowledged");
        mesh_schedule_tx_timeout();
    }
    if (result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) {
        LOG_INF("mesh local delivery ready");
    }
    if (DEVICE_ROLE == ROLE_ANCHOR && !mesh_relay_tx_active(&mesh_runtime)) {
        report_tx_schedule(0u);
    }
    if (!mesh_relay_tx_active(&mesh_runtime)) {
        mesh_try_route_waiting_tx();
    }
}

static void mesh_rx_work_handler(struct k_work *work)
{
    struct mesh_rx_pending *pending = &mesh_rx_work_pending;
    struct mesh_relay_result *result = &mesh_work_result;
    int ret;

    ARG_UNUSED(work);

    while (k_msgq_get(&mesh_rx_msgq, pending, K_NO_WAIT) == 0) {
        uint32_t now_ms = k_uptime_get_32();
        bool handled_event_control = false;

        memset(result, 0, sizeof(*result));
        mesh_rx_pending_refresh_age(pending, now_ms);
        /* A peer TX slot can carry ACKs, telemetry, or control; ACK matching is opportunistic. */
        (void)mesh_ch9_tx_pending_handle_ack(&pending->packet,
                                             pending->payload,
                                             pending->payload_len);
        if (mesh_gateway_route_test_should_reject_route_request(pending)) {
            continue;
        }
        ret = mesh_relay_handle_rx(&mesh_runtime,
                                   &pending->packet,
                                   pending->payload,
                                   pending->payload_len,
                                   pending->previous_hop_id,
                                   pending->link_quality,
                                   now_ms,
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
        mesh_handle_result_actions(result, pending->radio_channel, pending);
        if (handled_event_control) {
            continue;
        }
        if ((result->actions & MESH_RELAY_ACTION_DELIVER_LOCAL) != 0u &&
            DEVICE_ROLE == ROLE_GATEWAY) {
            if (pending->packet.msg_type == MSG_COMMAND_RESULT) {
                gateway_note_command_result(&pending->packet,
                                            pending->payload,
                                            pending->payload_len);
            }
            mesh_report_gateway_handle_survey_discovery_report(&pending->packet,
                                                   pending->payload,
                                                   pending->payload_len);
            ret = gateway_emit_host_packet(&pending->packet,
                                           pending->payload,
                                           pending->payload_len);
            if (ret < 0) {
                LOG_WRN("gateway BLE COBS frame not emitted: %d", ret);
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
    }
}

static void mesh_ch9_tx_pending_handle_timeout(uint32_t now_ms)
{
    uint64_t timed_out_next_hop_id = 0u;
    uint8_t requeued = 0u;
    uint8_t acked = 0u;

    if (!mesh_ch9_tx_pending.active ||
        !uptime_deadline_reached(now_ms, mesh_ch9_tx_pending.deadline_ms)) {
        return;
    }

    timed_out_next_hop_id = mesh_ch9_tx_pending.next_hop_id;
    for (uint8_t i = 0u; i < mesh_ch9_tx_pending.count; i++) {
        const struct mesh_ch9_tx_pending_entry *entry = &mesh_ch9_tx_pending.entries[i];

        if (entry->acked) {
            acked++;
            continue;
        }
        if (queue_anchor_report(&entry->outbound) == 0) {
            requeued++;
        } else {
            HIGH_DEBUG_COUNTER_INC(mesh_drop);
        }
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_note("DBG_CH9_TX_ACK_TIMEOUT\n");
        status_debug_printf("DBG_CH9_TX_ACK_TIMEOUT n=%u acked=%u requeued=%u now=%u deadline=%u\n",
                            mesh_ch9_tx_pending.count,
                            acked,
                            requeued,
                            now_ms,
                            mesh_ch9_tx_pending.deadline_ms);
    }
    LOG_WRN("mesh channel-9 TX batch ACK timeout: count=%u acked=%u requeued=%u",
            mesh_ch9_tx_pending.count,
            acked,
            requeued);
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
    mesh_ch9_tx_pending_clear();
}

static void mesh_tx_timeout_handler(struct k_work *work)
{
    struct mesh_relay_result *result = &mesh_work_result;
    struct mesh_outbound *pending_waiting = &mesh_tx_timeout_pending_waiting;
    bool pending_route_waiting = false;
    struct mesh_outbound *pending_report = &mesh_tx_timeout_pending_report;
    bool pending_anchor_report = false;

    ARG_UNUSED(work);
    memset(result, 0, sizeof(*result));
    memset(pending_waiting, 0, sizeof(*pending_waiting));
    memset(pending_report, 0, sizeof(*pending_report));

    mesh_ch9_tx_pending_handle_timeout(k_uptime_get_32());

    if (DEVICE_ROLE == ROLE_ANCHOR && mesh_report_anchor_survey_discovery_is_pending()) {
        (void)mesh_reschedule_delayable(&mesh_tx_timeout_work, REPORT_TX_RETRY_DELAY_MS);
        return;
    }

    if (DEVICE_ROLE == ROLE_ANCHOR &&
        mesh_relay_tx_active(&mesh_runtime) &&
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
    mesh_handle_result_actions(result, UWB_CHANNEL_WAKE_CONTACT, NULL);
    if (mesh_relay_tx_active(&mesh_runtime)) {
        mesh_schedule_tx_timeout();
    }

    if (pending_route_waiting &&
        (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        int route_ret;

        mesh_store_route_waiting_tx(pending_waiting);
        route_ret = mesh_request_route(pending_waiting->packet.dst_id, "pending-tx-timeout");
        if (route_ret == -ETIMEDOUT) {
            mesh_drop_route_waiting_tx("pending-tx-route-exhausted");
        }
    }

    if (pending_anchor_report &&
        (result->actions & MESH_RELAY_ACTION_ROUTE_DISCOVERY_NEEDED) != 0u) {
        LOG_WRN("requeueing click report after mesh route loss");
        (void)queue_anchor_report(pending_report);
    }
}

static bool mesh_queue_from_frame_at(const uint8_t *frame,
                                     size_t frame_len,
                                     uint8_t link_quality,
                                     uint8_t radio_channel,
                                     uint32_t received_at_ms,
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
        return false;
    }
    if (valid_mesh_frame != NULL) {
        *valid_mesh_frame = true;
    }
    if (previous_hop_id != NULL) {
        *previous_hop_id = context.previous_hop_id;
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
    app_mesh_test_note_wake_event(&pending.packet,
                                  pending.previous_hop_id,
                                  pending.link_quality,
                                  radio_channel);

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

    (void)mesh_submit_work(&mesh_rx_work);
    return true;
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
                                    valid_mesh_frame,
                                    previous_hop_id);
}

static bool mesh_handle_channel5_wake_claim(const uint8_t *frame,
                                            size_t frame_len,
                                            uint8_t link_quality)
{
    struct uwb_wake_claim_frame claim;
    int ret;

    if (frame == NULL || frame_len == 0u) {
        return false;
    }

    ret = uwb_decode_wake_claim(frame, frame_len, &claim);
    if (ret != PROTO_OK) {
        return false;
    }

    app_mesh_test_note_wake_claim(claim.clicker_id,
                                  claim.click_event_id,
                                  claim.attempt_index,
                                  link_quality);
    mesh_gateway_route_test_note_channel5_contact(claim.clicker_id, "wake-claim");
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

    if (!channel9_event &&
        mesh_handle_channel5_wake_claim(frame, frame_len, quality)) {
        LOG_DBG("mesh UWB RX accepted channel-5 wake claim: len=%u",
                (unsigned int)frame_len);
        return true;
    }

    if (mesh_queue_from_frame(frame,
                              frame_len,
                              quality,
                              channel9_event ? UWB_CHANNEL_MESH_PAYLOAD :
                                               UWB_CHANNEL_WAKE_CONTACT,
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
	        } else if (channel9_event && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
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
    uint32_t channel5_gap_window_ms = 0u;
    int ret;

    ARG_UNUSED(work);

    if (!mesh_role_uses_uwb_rx()) {
        mesh_uwb_rx_active = false;
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

    gateway_route_preempt = mesh_gateway_route_test_preempt_active(k_uptime_get_32());
    gateway_continuous_ch5 = mesh_gateway_route_test_role() &&
                             mesh_channel9_connection_count() == 0u;
    channel9_event = mesh_select_channel9_ack_tx_event(k_uptime_get_32(),
                                                       &channel9_plan,
                                                       &channel9_peer_id);
    if (channel9_event) {
        (void)mesh_send_pending_ch9_ack_batch(&channel9_plan,
                                              channel9_peer_id,
                                              "ch9-ack-batch-slot");
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
        return;
    }

    channel9_event = mesh_select_channel9_rx_event(k_uptime_get_32(),
                                                   &channel9_plan,
                                                   &channel9_peer_id,
                                                   &channel9_timing_index);
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
            if (next_delay_ms > 0u) {
                next_delay_ms++;
            }
            mesh_schedule_uwb_rx(next_delay_ms);
            return;
        }
        channel5_gap_scan = true;
    }

    ret = radio_guard_uwb_start(channel9_event ? "mesh channel9 UWB RX" : "mesh UWB RX");
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
    uwb_window_start_ms = k_uptime_get();
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
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
                                MESH_EVENT_DEFAULT_GUARD_MS);
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
        (void)dwm3000_driver_standby();
    }
    mesh_report_note_anchor_uwb_awake_since(uwb_window_start_ms, 0u);
    radio_guard_uwb_stop();

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
	    } else if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) && channel9_event) {
	        status_debug_note(channel9_peer_observed ?
	                          "DBG_CH9_RX_DONE_WITH_PEER\n" :
	                          "DBG_CH9_RX_TIMEOUT\n");
	    }

    if (mesh_gateway_route_test_preempt_active(k_uptime_get_32()) ||
        (mesh_gateway_route_test_role() &&
         mesh_channel9_connection_count() == 0u)) {
        mesh_schedule_uwb_rx(0u);
    } else {
        mesh_schedule_uwb_rx(mesh_next_channel9_rx_delay_ms(k_uptime_get_32()));
    }
}

int mesh_start_uwb_rx(const char *reason)
{
    if (!mesh_role_uses_uwb_rx()) {
        return -EINVAL;
    }

    mesh_schedule_uwb_rx(0u);
    LOG_INF("mesh UWB RX scheduled: role=%s window_ms=%u idle_ms=%u reason=%s",
            role_name(),
            mesh_uwb_rx_window_ms(),
            mesh_uwb_rx_idle_delay_ms(),
            reason == NULL ? "start" : reason);
    return 0;
}

static int build_range_report_samples(uint64_t clicker_id,
                                       uint32_t event_seq,
                                       uint32_t burst_id,
                                       const struct dwm3000_range_result *range_result,
                                      const int32_t *distance_samples_mm,
                                      const uint8_t *range_round_indices,
                                      const int64_t *sample_sequence_start_ms,
                                      uint16_t sample_count)
{
    struct range_report_fields fields;
    struct proto_packet packet;
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_MAX_LEN];
    uint16_t sample_index = 0u;
    uint16_t packet_index = 0u;
    bool fragmented;
    int ret;

    if (range_result == NULL ||
        clicker_id == 0u ||
        event_seq == 0u ||
        range_result->responder_id == 0u ||
        (sample_count > 0u &&
         (distance_samples_mm == NULL ||
          range_round_indices == NULL ||
          sample_sequence_start_ms == NULL)) ||
        sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES) {
        return -EINVAL;
    }
    fragmented = sample_count > RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;

    do {
        struct range_report_diagnostics diagnostics;
        size_t payload_len = 0u;
        size_t encoded_len = 0u;
        uint16_t chunk_count = 0u;
        uint16_t chunk_cap = 0u;
        uint16_t packet_seq;
        uint64_t sequence_start_timestamps_ms[RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET] = {0};
        int64_t range_local_ms;

        if (sample_count > 0u) {
            chunk_cap = fragmented ?
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT :
                        RANGE_REPORT_MAX_DISTANCE_SAMPLES_SINGLE_PACKET;
            chunk_count = MIN(chunk_cap, sample_count - sample_index);
        }

build_payload:
        payload_len = 0u;
        encoded_len = 0u;
        memset(&fields, 0, sizeof(fields));
        if (chunk_count > 0u) {
            range_local_ms = sample_sequence_start_ms[sample_index];
            if (range_local_ms < 0) {
                range_local_ms = k_uptime_get();
            }
            for (uint16_t i = 0u; i < chunk_count; i++) {
                int64_t sample_local_ms = sample_sequence_start_ms[sample_index + i];

                if (sample_local_ms < 0) {
                    sample_local_ms = k_uptime_get();
                }
                anchor_sequence_timestamp_at(sample_local_ms,
                                             &sequence_start_timestamps_ms[i]);
            }
            fields.timestamp_ms = sequence_start_timestamps_ms[0];
        } else {
            range_local_ms = range_result->exchange_started ?
                             range_result->exchange_start_ms :
                             k_uptime_get();
            anchor_sequence_timestamp_at(range_local_ms,
                                         &fields.timestamp_ms);
        }

        fields.clicker_id = clicker_id;
        fields.anchor_id = range_result->responder_id;
        fields.event_seq = event_seq;
        fields.distance_mm = range_result->distance_mm;
        fields.quality = range_result->quality;
        fields.rsl_dbm = range_result->rsl_dbm;
        fields.cir_sample = range_result->cir_sampled ? range_result->cir_sample : NULL;
        fields.range_status = range_result->status;
        fields.distance_samples_mm = chunk_count > 0u ? &distance_samples_mm[sample_index] : NULL;
        fields.range_round_indices = chunk_count > 0u ?
                                     &range_round_indices[sample_index] :
                                     NULL;
        fields.sequence_start_timestamps_ms = chunk_count > 0u ?
                                              sequence_start_timestamps_ms :
                                              NULL;
        fields.sample_index = sample_index;
        fields.sample_count = sample_count;
        fields.distance_sample_count = chunk_count;
        fields.omit_rsl = packet_index != 0u;
        fields.omit_cir = packet_index != 0u;
        fragmented = sample_count > chunk_count || packet_index != 0u;
        if (packet_index == 0u) {
            uint32_t anchor_diag_len = range_result->cir_sampled ?
                                       UWB_CIR_SAMPLE_LEN : 0u;
            uint32_t clicker_diag_len = range_result->clicker_diag_received ?
                                        range_result->clicker_diag_len : 0u;
            uint32_t clicker_diag_copy_len = clicker_diag_len > 15u ?
                                             clicker_diag_len - 15u : 0u;
            uint32_t clicker_diag_raw_len =
                (range_result->clicker_diag_received && clicker_diag_len >= 15u) ?
                range_result->clicker_diag[14] : clicker_diag_copy_len;

            memset(&diagnostics, 0, sizeof(diagnostics));
            diagnostics.status_flags = range_result->cir_sampled ?
                                       RANGE_DIAG_ANCHOR_PRESENT :
                                       RANGE_DIAG_ANCHOR_MISSING;
            diagnostics.status_flags |= range_result->clicker_diag_received ?
                                        RANGE_DIAG_CLICKER_PRESENT :
                                        RANGE_DIAG_CLICKER_MISSING;
            if (range_result->clicker_diag_truncated) {
                diagnostics.status_flags |= RANGE_DIAG_TRUNCATED;
            }
            if (range_result->clicker_diag_dropped) {
                diagnostics.status_flags |= RANGE_DIAG_CAPTURE_FAILED;
            }
            diagnostics.burst_id = burst_id;
            diagnostics.exchange_stride_us = UWB_RANGE_SCHEDULE_MIN_EXCHANGE_STRIDE_US;
            diagnostics.burst_duration_ms = UWB_RANGE_SCHEDULE_DEFAULT_BURST_WINDOW_MS;
            diagnostics.uwb_awake_time_us = anchor_uwb_session.diagnostics.awake_time_us;
            diagnostics.diag_bytes_captured = anchor_diag_len + clicker_diag_raw_len;
            diagnostics.diag_bytes_transmitted = anchor_diag_len + clicker_diag_copy_len;
            diagnostics.diag_bytes_truncated = clicker_diag_raw_len > clicker_diag_copy_len ?
                                               clicker_diag_raw_len - clicker_diag_copy_len :
                                               0u;
            diagnostics.diag_frames_dropped = range_result->clicker_diag_dropped ?
                                              1u : 0u;
            diagnostics.report_fragment_count = fragmented ?
                                                (uint16_t)(1u +
                                                           ((sample_count - chunk_count +
                                                             RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT -
                                                             1u) /
                                                            RANGE_REPORT_MAX_DISTANCE_SAMPLES_FRAGMENT)) :
                                                1u;
            diagnostics.phy_config_id = UWB_CHANNEL_WAKE_CONTACT;
            diagnostics.clock_offset_raw = range_result->clock_offset_raw;
            diagnostics.clock_offset_present = range_result->clock_offset_sampled;
            diagnostics.carrier_integrator = range_result->carrier_integrator;
            diagnostics.carrier_integrator_present =
                range_result->carrier_integrator_sampled;
            diagnostics.clicker_diag = range_result->clicker_diag_received ?
                                       range_result->clicker_diag : NULL;
            diagnostics.clicker_diag_len = range_result->clicker_diag_received ?
                                           range_result->clicker_diag_len : 0u;
            diagnostics.anchor_diag = range_result->cir_sampled ?
                                      range_result->cir_sample : NULL;
            diagnostics.anchor_diag_len = range_result->cir_sampled ?
                                          UWB_CIR_SAMPLE_LEN : 0u;
            fields.diagnostics = &diagnostics;
        }

        ret = report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields);
        if (ret == PROTO_ERR_NO_SPACE && chunk_count > 1u) {
            chunk_count--;
            goto build_payload;
        }
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        packet_seq = (uint16_t)((range_result->seq == 0u ?
                                 (uint16_t)event_seq :
                                 range_result->seq) + packet_index);
        ret = report_init_range_packet(&packet,
                                       range_result->responder_id,
                                       GATEWAY_ID,
                                       event_seq,
                                       packet_seq,
                                       range_result->flags,
                                       (uint8_t)payload_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        ret = proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len);
        if (ret != PROTO_OK) {
            return -EINVAL;
        }

        LOG_INF("range report ready: clicker=0x%016llx event_seq=%u anchor=0x%016llx distance_mm=%d samples=%u chunk_index=%u chunk_samples=%u first_round=%u quality=%u diagnostic=%u rsl_included=%u packet_len=%u",
                (unsigned long long)clicker_id,
                event_seq,
                (unsigned long long)range_result->responder_id,
                range_result->distance_mm,
                sample_count,
                sample_index,
                chunk_count,
                chunk_count > 0u ? range_round_indices[sample_index] : 0u,
                range_result->quality,
                (range_result->flags & FLAG_DIAGNOSTIC) != 0u ? 1u : 0u,
                packet_index == 0u ? 1u : 0u,
                (unsigned int)encoded_len);

        if (DEVICE_ROLE == ROLE_ANCHOR) {
            struct mesh_outbound outbound = {
                .packet = packet,
                .payload_len = (uint8_t)payload_len,
            };

            memcpy(outbound.payload, payload, payload_len);
            ret = queue_anchor_report(&outbound);
            if (ret < 0) {
                LOG_WRN("click report could not be queued for mesh TX: %d", ret);
                return ret;
            }
        }

        sample_index += chunk_count;
        packet_index++;
    } while (sample_index < sample_count);

    return 0;
}

void build_uwb_schedule_report_if_relevant(
    const struct uwb_anchor_session *session,
    uint8_t schedule_flags,
    const struct anchor_range_window_report *report)
{
    int ret;

    if ((schedule_flags & (FLAG_COUNT_AS_CLICK | FLAG_DIAGNOSTIC)) == 0u) {
        return;
    }
    if (session == NULL || report == NULL || !report->have_result) {
        return;
    }

    ret = build_range_report_samples(session->epoch.clicker_id,
                                     session->epoch.click_event_id,
                                     uwb_schedule_burst_id(session->epoch.click_event_id,
                                                           session->epoch.attempt_index),
                                     &report->result,
                                     report->distance_samples_mm,
                                     report->range_round_indices,
                                     report->sample_sequence_start_ms,
                                     report->sample_count);
    if (ret < 0) {
        LOG_WRN("failed to build UWB scheduled anchor range report: %d", ret);
    }
}

int app_mesh_report_init(const struct app_mesh_report_callbacks *callbacks)
{
    mesh_report_callbacks = callbacks;
#if defined(CONFIG_IMEC_MESH_ROUTE_TEST) && \
    !defined(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)
    k_work_queue_start(&mesh_route_work_q,
                       mesh_route_work_q_stack,
                       K_THREAD_STACK_SIZEOF(mesh_route_work_q_stack),
                       MESH_ROUTE_WORKQUEUE_PRIORITY,
                       &mesh_route_work_q_config);
#endif
    k_work_init(&mesh_rx_work, mesh_rx_work_handler);
    k_work_init_delayable(&mesh_uwb_rx_work, mesh_uwb_rx_work_handler);
    k_work_init_delayable(&mesh_tx_timeout_work, mesh_tx_timeout_handler);
    k_work_init_delayable(&report_tx_work, report_tx_work_handler);
    return 0;
}
