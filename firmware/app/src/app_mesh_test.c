#include "app_mesh_test.h"

#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_report.h"
#include "app_mesh_smoke_fast.h"
#include "app_board.h"
#include "app_state.h"
#include "mesh_relay.h"
#include "uwb.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_mesh_test, LOG_LEVEL_DBG);

#if defined(CONFIG_IMEC_MESH_ROUTE_TEST)
#define MESH_TEST_FLAG_GATEWAY_ACK_REQUIRED (1u << 0)
#define MESH_TEST_FLAG_CHANNEL9_PAYLOAD     (1u << 1)
#define MESH_TEST_FLAG_CH5_WAKE_CONTINUOUS  (1u << 2)
#define MESH_TEST_SUMMARY_INTERVAL_MS 5000u

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS 1000
#endif

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT 1
#endif

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES
#define CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES 0
#endif

BUILD_ASSERT(CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT <= REPORT_TX_QUEUE_DEPTH,
             "mesh-test TX burst must fit in the report TX queue");
BUILD_ASSERT(CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES <= UWB_MESH_MAX_PAYLOAD_LEN,
             "mesh-test payload must fit in the mesh payload buffer");

K_THREAD_STACK_DEFINE(mesh_test_thread_stack, MESH_TEST_WORKQUEUE_STACK_SIZE);
static struct k_thread mesh_test_thread;
static bool mesh_test_thread_started;
static uint32_t mesh_test_next_packet_id = 1u;
static uint16_t mesh_test_attempt;
static uint16_t mesh_test_seq;
static uint32_t mesh_test_drop_count;
static bool mesh_test_ch5_refresh_logged;
static uint8_t mesh_test_wait_log_ticks;
static struct mesh_smoke_fast_state mesh_test_gateway_state;
static uint32_t mesh_test_gateway_last_summary_ms;

static uint8_t mesh_test_ch9_state_for_parent(uint64_t parent_id)
{
    bool route_selected = parent_id != 0u;

    if (!route_selected) {
        return MESH_SMOKE_FAST_CH9_NONE;
    }
    for (uint8_t i = 0u; i < MESH_RELAY_EVENT_TIMINGS; i++) {
        const struct mesh_relay_event_timing_entry *entry =
            &mesh_runtime.event_timings[i];

        if (!entry->valid || entry->next_hop_id != parent_id) {
            continue;
        }
        if (entry->timing.timing_fresh && entry->timing.route_fresh) {
            return MESH_SMOKE_FAST_CH9_TIMING_FRESH;
        }
        return MESH_SMOKE_FAST_CH9_TIMING_STALE;
    }
    return MESH_SMOKE_FAST_CH9_ROUTE_ONLY;
}

static uint64_t mesh_test_selected_parent(void)
{
    uint64_t next_hop_id = 0u;

    if (mesh_relay_select_next_hop(&mesh_runtime,
                                   GATEWAY_ID,
                                   &next_hop_id) != PROTO_OK) {
        return 0u;
    }
    return next_hop_id;
}

static uint16_t mesh_test_next_seq(void)
{
    mesh_test_seq++;
    if (mesh_test_seq == 0u) {
        mesh_test_seq = 1u;
    }
    return mesh_test_seq;
}

static uint16_t mesh_test_next_attempt(void)
{
    if (mesh_test_attempt < UINT16_MAX) {
        mesh_test_attempt++;
    }
    if (mesh_test_attempt == 0u) {
        mesh_test_attempt = 1u;
    }
    return mesh_test_attempt;
}

static void mesh_test_reset_attempts(void)
{
    mesh_test_attempt = 0u;
}

static void mesh_test_advance_packet_id(void)
{
    mesh_test_next_packet_id++;
    if (mesh_test_next_packet_id == 0u) {
        mesh_test_next_packet_id = 1u;
    }
}

static int mesh_test_append_payload(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    uint32_t packet_id,
                                    uint16_t attempt)
{
    const uint64_t selected_parent = mesh_test_selected_parent();
    const uint32_t flags = MESH_TEST_FLAG_GATEWAY_ACK_REQUIRED |
                           MESH_TEST_FLAG_CHANNEL9_PAYLOAD |
                           MESH_TEST_FLAG_CH5_WAKE_CONTINUOUS;
    const struct mesh_smoke_fast_payload_input input = {
        .packet_id = packet_id,
        .build_uptime_ms = k_uptime_get_32(),
        .packet_age_ms = 0u,
        .drop_count = mesh_test_drop_count,
        .origin_id = DEVICE_ID,
        .target_id = GATEWAY_ID,
        .selected_parent_id = selected_parent,
        .attempt = attempt,
        .device_role = (uint8_t)DEVICE_ROLE,
        .mesh_channel = UWB_CHANNEL_MESH_PAYLOAD,
        .ch9_timing_state = mesh_test_ch9_state_for_parent(selected_parent),
        .flags = flags,
    };

    return mesh_smoke_fast_payload_append(payload,
                                          payload_cap,
                                          payload_len,
                                          &input,
                                          CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES);
}

static int mesh_test_build_packet(struct mesh_outbound *outbound,
                                  uint32_t packet_id,
                                  uint16_t attempt)
{
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL) {
        return -EINVAL;
    }

    memset(outbound, 0, sizeof(*outbound));
    ret = mesh_test_append_payload(outbound->payload,
                                   sizeof(outbound->payload),
                                   &payload_len,
                                   packet_id,
                                   attempt);
    if (ret != PROTO_OK) {
        return mesh_errno_from_proto(ret);
    }

    outbound->packet.msg_type = MSG_MESH_DATA;
    outbound->packet.flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC;
    outbound->packet.src_id = DEVICE_ID;
    outbound->packet.dst_id = GATEWAY_ID;
    outbound->packet.session_id = nonzero_uptime_session_id();
    outbound->packet.seq = mesh_test_next_seq();
    outbound->packet.ttl = MESH_DEFAULT_TTL;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint16_t)payload_len;
    return 0;
}

static uint32_t mesh_test_tx_once(void)
{
    struct mesh_outbound outbound;
    uint32_t packet_id;
    uint16_t attempt;
    bool relay_tx_active;
    bool route_waiting_active;
    bool ack_wait_active;
    uint32_t queue_used;
    uint8_t burst_target;
    struct mesh_smoke_fast_tx_gate gate;
    struct mesh_smoke_fast_tx_decision decision;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS;
    }
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    route_waiting_active = mesh_route_waiting_tx_active();
    ack_wait_active = mesh_report_ch9_ack_wait_active();
    queue_used = report_tx_queue_used();
    gate = (struct mesh_smoke_fast_tx_gate) {
        .relay_tx_active = relay_tx_active,
        .route_waiting_active = route_waiting_active,
        .ack_wait_active = ack_wait_active,
        .queue_used = queue_used,
        .queue_depth = REPORT_TX_QUEUE_DEPTH,
        .configured_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS,
        .fast_mode = IS_ENABLED(CONFIG_MESH_SMOKE_FAST_TX),
    };
    mesh_smoke_fast_tx_decide(&gate, &decision);
    if (!decision.can_queue) {
        if (mesh_test_wait_log_ticks == 0u || mesh_test_wait_log_ticks >= 10u) {
            status_debug_note("DBG_MESH_TEST_WAIT\n");
            if (relay_tx_active) {
                status_debug_note("DBG_MESH_TEST_WAIT_RELAY\n");
            }
            if (route_waiting_active) {
                status_debug_note("DBG_MESH_TEST_WAIT_ROUTE\n");
            }
            if (decision.reason == MESH_SMOKE_FAST_DEFER_QUEUE_FULL) {
                status_debug_note("DBG_MESH_TEST_WAIT_BACKLOG\n");
            }
            if (ack_wait_active) {
                status_debug_note("DBG_MESH_TEST_WAIT_ACK\n");
            }
            if (queue_used > 0u) {
                status_debug_note("DBG_MESH_TEST_QUEUE_WAIT\n");
            }
            status_debug_printf("DBG_MESH_TEST_WAIT_STATE relay=%u route=%u headroom=%u ack=%u q=%u id=%u att=%u\n",
                                relay_tx_active ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                decision.queue_headroom,
                                ack_wait_active ? 1u : 0u,
                                queue_used,
                                mesh_test_next_packet_id,
                                mesh_test_attempt);
            mesh_test_wait_log_ticks = 1u;
        } else {
            mesh_test_wait_log_ticks++;
        }
        return decision.delay_ms;
    }
    mesh_test_wait_log_ticks = 0u;
    burst_target = (uint8_t)MIN((uint32_t)CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT,
                               decision.queue_headroom);

    for (uint8_t queued_count = 0u;
         queued_count < burst_target;
         queued_count++) {
        packet_id = mesh_test_next_packet_id;
        attempt = mesh_test_next_attempt();
        ret = mesh_test_build_packet(&outbound, packet_id, attempt);
        if (ret < 0) {
            mesh_test_drop_count++;
            LOG_WRN("mesh-test packet build failed: id=%u attempt=%u ret=%d drops=%u",
                    packet_id, attempt, ret, mesh_test_drop_count);
            return decision.delay_ms;
        }

        ret = queue_anchor_report(&outbound);
        status_debug_note(ret == 0 ? "DBG_MESH_TEST_SEND_OK\n" :
                          "DBG_MESH_TEST_SEND_FAIL\n");
        if (ret == 0) {
            mesh_test_advance_packet_id();
            mesh_test_reset_attempts();
            status_debug_printf("DBG_MESH_TEST_QUEUED id=%u att=%u q=%u burst=%u/%u\n",
                                packet_id,
                                attempt,
                                report_tx_queue_used(),
                                (uint8_t)(queued_count + 1u),
                                burst_target);
            continue;
        }

        mesh_test_drop_count++;
        high_debug_log_event("MESH_TEST_TX_RETRY",
                             "id=%u attempt=%u ret=%d drops=%u queued=%u",
                             packet_id,
                             attempt,
                             ret,
                             mesh_test_drop_count,
                             queued_count);
        LOG_WRN("mesh-test synthetic packet not launched: id=%u attempt=%u ret=%d drops=%u queued=%u",
                packet_id, attempt, ret, mesh_test_drop_count, queued_count);
        return ret == -EBUSY ? 100u : 250u;
    }

    return decision.delay_ms;
}

static void mesh_test_tx_thread_entry(void *arg0, void *arg1, void *arg2)
{
    uint32_t delay_ms = CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS + 500u;

    ARG_UNUSED(arg0);
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);

    status_debug_note("DBG_MESH_TEST_THREAD\n");
    k_msleep(delay_ms);
    for (;;) {
        delay_ms = mesh_test_tx_once();
        k_msleep(delay_ms);
    }
}

int app_mesh_test_init(void)
{
    return 0;
}

int app_mesh_test_start(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return 0;
    }

    (void)app_mesh_test_init();
    status_debug_note("DBG_MESH_TEST_START\n");
    if (!mesh_test_thread_started) {
        k_tid_t tid = k_thread_create(&mesh_test_thread,
                                      mesh_test_thread_stack,
                                      K_THREAD_STACK_SIZEOF(mesh_test_thread_stack),
                                      mesh_test_tx_thread_entry,
                                      NULL,
                                      NULL,
                                      NULL,
                                      MESH_TEST_WORKQUEUE_PRIORITY,
                                      0,
                                      K_NO_WAIT);

        k_thread_name_set(tid, "mesh_test");
        mesh_test_thread_started = true;
    }
    LOG_INF("mesh-test synthetic transmitter enabled: dst=0x%016llx interval_ms=%u burst=%u payload_bytes=%u startup_delay_ms=%u fast=%u",
            (unsigned long long)GATEWAY_ID,
            CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS,
            CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT,
            CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES,
            CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS,
            IS_ENABLED(CONFIG_MESH_SMOKE_FAST_TX) ? 1u : 0u);
    return 0;
}

void app_mesh_test_note_wake_event(const struct proto_packet *packet,
                                   uint64_t previous_hop_id,
                                   uint8_t link_quality,
                                   uint8_t radio_channel)
{
    uint32_t target_scan_interval_ms;

    if (DEVICE_ROLE != ROLE_ANCHOR || packet == NULL ||
        radio_channel != UWB_CHANNEL_WAKE_CONTACT ||
        (packet->dst_id != MESH_BROADCAST_ID && packet->dst_id != DEVICE_ID)) {
        return;
    }

    target_scan_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS;
    if (anchor_uwb_scan_interval_ms != target_scan_interval_ms) {
        anchor_uwb_scan_interval_ms = target_scan_interval_ms;
    }
    if (!mesh_test_ch5_refresh_logged) {
        mesh_test_ch5_refresh_logged = true;
        LOG_INF("mesh-test wake event refreshed anchor channel-5 scan: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u interval_ms=%u",
                packet->msg_type,
                (unsigned long long)packet->src_id,
                (unsigned long long)packet->dst_id,
                (unsigned long long)previous_hop_id,
                link_quality,
                anchor_uwb_scan_interval_ms);
    }
    high_debug_log_event("MESH_TEST_WAKE",
                         "msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u ch5_scan_interval_ms=%u",
                         packet->msg_type,
                         (unsigned long long)packet->src_id,
                         (unsigned long long)packet->dst_id,
                         (unsigned long long)previous_hop_id,
                         link_quality,
                         anchor_uwb_scan_interval_ms);
    mesh_smoke_fast_note_c5_refresh(&mesh_test_gateway_state);
}

void app_mesh_test_note_wake_claim(uint64_t source_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint8_t link_quality)
{
    uint32_t target_scan_interval_ms;

    if (DEVICE_ROLE != ROLE_ANCHOR) {
        return;
    }

    target_scan_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS;
    if (anchor_uwb_scan_interval_ms != target_scan_interval_ms) {
        anchor_uwb_scan_interval_ms = target_scan_interval_ms;
    }
    if (!mesh_test_ch5_refresh_logged) {
        mesh_test_ch5_refresh_logged = true;
        LOG_INF("mesh-test wake claim refreshed anchor channel-5 scan: src=0x%016llx event_seq=%u attempt=%u quality=%u interval_ms=%u",
                (unsigned long long)source_id,
                event_seq,
                attempt_index,
                link_quality,
                anchor_uwb_scan_interval_ms);
    }
    high_debug_log_event("MESH_TEST_WAKE_CLAIM",
                         "src=0x%016llx event_seq=%u attempt=%u quality=%u ch5_scan_interval_ms=%u",
                         (unsigned long long)source_id,
                         event_seq,
                         attempt_index,
                         link_quality,
                         anchor_uwb_scan_interval_ms);
    mesh_smoke_fast_note_c5_refresh(&mesh_test_gateway_state);
}

void app_mesh_test_note_ch9_missed(void)
{
    mesh_smoke_fast_note_ch9_missed(&mesh_test_gateway_state);
}

void app_mesh_test_note_gateway_delivery(const struct proto_packet *packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint32_t received_at_ms,
                                         uint32_t queue_depth)
{
    struct mesh_smoke_fast_summary summary;
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    if (DEVICE_ROLE != ROLE_GATEWAY || packet == NULL ||
        packet->msg_type != MSG_MESH_DATA) {
        return;
    }
    ret = mesh_smoke_fast_note_delivery(&mesh_test_gateway_state,
                                        payload,
                                        payload_len,
                                        now_ms,
                                        now_ms - received_at_ms,
                                        queue_depth);
    if (ret == PROTO_ERR_NOT_FOUND) {
        return;
    }
    if (ret < 0) {
        LOG_WRN("mesh-smoke gateway verify failed: ret=%d src=0x%016llx seq=%u",
                ret,
                (unsigned long long)packet->src_id,
                packet->seq);
        return;
    }
    if (mesh_test_gateway_last_summary_ms != 0u &&
        now_ms - mesh_test_gateway_last_summary_ms < MESH_TEST_SUMMARY_INTERVAL_MS) {
        return;
    }
    mesh_test_gateway_last_summary_ms = now_ms;
    mesh_smoke_fast_get_summary(&mesh_test_gateway_state, &summary);
    LOG_INF("mesh-smoke summary throughput=%u delivered=%u dup=%u gaps=%u missing=%u late_missing=%u attributed=%u retries=%u retry_max=%u ch9_miss=%u c5_refresh=%u ack_lat_ms=%u/%u/%u qmax=%u last_id=%u drop_reason=%u",
            summary.last_delivered_ms > summary.first_delivered_ms ?
            (summary.delivered_count * 1000u) /
            (summary.last_delivered_ms - summary.first_delivered_ms) :
            summary.delivered_count,
            summary.delivered_count,
            summary.duplicate_count,
            summary.gap_count,
            summary.missing_count,
            summary.later_delivered_missing_count,
            summary.attributed_missing_count,
            summary.retry_total,
            summary.retry_max,
            summary.missed_ch9_events,
            summary.c5_refreshes,
            summary.gateway_ack_latency_p50_ms,
            summary.gateway_ack_latency_p95_ms,
            summary.gateway_ack_latency_max_ms,
            summary.queue_depth_max,
            summary.last_packet_id,
            summary.last_drop_or_defer_reason);
}
#else
int app_mesh_test_init(void)
{
    return 0;
}

int app_mesh_test_start(void)
{
    return 0;
}

void app_mesh_test_note_wake_event(const struct proto_packet *packet,
                                   uint64_t previous_hop_id,
                                   uint8_t link_quality,
                                   uint8_t radio_channel)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(previous_hop_id);
    ARG_UNUSED(link_quality);
    ARG_UNUSED(radio_channel);
}

void app_mesh_test_note_wake_claim(uint64_t source_id,
                                   uint32_t event_seq,
                                   uint8_t attempt_index,
                                   uint8_t link_quality)
{
    ARG_UNUSED(source_id);
    ARG_UNUSED(event_seq);
    ARG_UNUSED(attempt_index);
    ARG_UNUSED(link_quality);
}

void app_mesh_test_note_ch9_missed(void)
{
}

void app_mesh_test_note_gateway_delivery(const struct proto_packet *packet,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         uint32_t received_at_ms,
                                         uint32_t queue_depth)
{
    ARG_UNUSED(packet);
    ARG_UNUSED(payload);
    ARG_UNUSED(payload_len);
    ARG_UNUSED(received_at_ms);
    ARG_UNUSED(queue_depth);
}
#endif
