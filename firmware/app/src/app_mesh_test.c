#include "app_mesh_test.h"

#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_report.h"
#include "app_mesh_smoke_fast.h"
#include "app_node_comm.h"
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
#define MESH_TEST_ROUTE_SUMMARY_REPEATS 8u
#define MESH_TEST_DELIVERY_DEADLINE_MS 60000u
#define MESH_TEST_RETRY_ADMISSION_MS 100u
#define MESH_TEST_SOURCE_DELIVERY_CAPACITY \
    (APP_NODE_COMM_MAX_DELIVERIES - \
     APP_NODE_COMM_PROTOCOL_RESERVED_DELIVERIES)

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS 1000
#endif

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT 1
#endif

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES
#define CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES 0
#endif

BUILD_ASSERT(CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES <= UWB_MESH_MAX_PAYLOAD_LEN,
             "mesh-test payload must fit in the mesh payload buffer");
BUILD_ASSERT(CONFIG_IMEC_MESH_ROUTE_TEST_PAYLOAD_BYTES <=
                 APP_NODE_COMM_FROZEN_PAYLOAD_MAX_LEN,
             "mesh-test payload must fit in communication custody");
BUILD_ASSERT(MESH_TEST_SOURCE_DELIVERY_CAPACITY >=
                 CONFIG_IMEC_MESH_ROUTE_TEST_TX_BURST_COUNT,
             "communication service must hold one complete source burst");

struct mesh_test_delivery_tracking {
    uint32_t handle;
    uint32_t packet_id;
    uint16_t seq;
};

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
static uint32_t mesh_test_first_queue_ms;
static uint32_t mesh_test_first_queue_id;
static uint16_t mesh_test_first_queue_seq;
static uint32_t mesh_test_queued_total;
static uint32_t mesh_test_retryable_before_first_ack;
static uint32_t mesh_test_backoff_before_first_ack;
static uint32_t mesh_test_ack_fail_before_first_success;
static bool mesh_test_first_ack_logged;
static uint32_t mesh_test_route_start_ms;
static uint32_t mesh_test_route_ready_ms;
static uint16_t mesh_test_route_reply_misses;
static uint16_t mesh_test_route_prep_backoffs;
static uint16_t mesh_test_direct_probe_successes;
static uint16_t mesh_test_direct_probe_failures;
static int32_t mesh_test_route_start_status;
static uint8_t mesh_test_route_start_attempts;
static uint8_t mesh_test_route_start_ttl;
static uint64_t mesh_test_route_next_hop_id;
static uint32_t mesh_test_route_next_summary_ms;
static uint8_t mesh_test_route_summary_repeats;
static bool mesh_test_route_start_seen;
static bool mesh_test_route_ready_logged;
static struct mesh_test_delivery_tracking
    mesh_test_deliveries[MESH_TEST_SOURCE_DELIVERY_CAPACITY];
static struct mesh_outbound mesh_test_pending_admission;
static uint64_t mesh_test_pending_admission_deadline_ms;
static uint32_t mesh_test_pending_admission_packet_id;
static uint16_t mesh_test_pending_admission_attempt;
static bool mesh_test_pending_admission_valid;

static void mesh_test_inc_u16(uint16_t *counter)
{
    if (counter != NULL && *counter < UINT16_MAX) {
        (*counter)++;
    }
}

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

static void mesh_test_note_queued(uint32_t packet_id, uint16_t seq)
{
    mesh_test_queued_total++;
    if (mesh_test_first_queue_ms == 0u) {
        mesh_test_first_queue_ms = k_uptime_get_32();
        mesh_test_first_queue_id = packet_id;
        mesh_test_first_queue_seq = seq;
    }
}

static struct mesh_test_delivery_tracking *mesh_test_free_delivery_slot(void)
{
    for (size_t i = 0u; i < MESH_TEST_SOURCE_DELIVERY_CAPACITY; i++) {
        if (mesh_test_deliveries[i].handle == 0u) {
            return &mesh_test_deliveries[i];
        }
    }
    return NULL;
}

static int mesh_test_track_delivery(uint32_t handle,
                                    uint32_t packet_id,
                                    uint16_t seq)
{
    struct mesh_test_delivery_tracking *slot = mesh_test_free_delivery_slot();

    if (slot == NULL || handle == 0u) {
        return -ENOSPC;
    }
    *slot = (struct mesh_test_delivery_tracking) {
        .handle = handle,
        .packet_id = packet_id,
        .seq = seq,
    };
    return 0;
}

static void mesh_test_reap_terminal_deliveries(void)
{
    for (size_t i = 0u; i < MESH_TEST_SOURCE_DELIVERY_CAPACITY; i++) {
        struct mesh_test_delivery_tracking *delivery =
            &mesh_test_deliveries[i];
        struct node_comm_terminal_event event;

        if (delivery->handle == 0u ||
            !app_node_comm_take_delivery_event_for(delivery->handle, &event)) {
            continue;
        }
        status_debug_printf(
            "DBG_MESH_TEST_TERMINAL id=%u seq=%u handle=%u reason=%u attempts=%u\n",
            delivery->packet_id,
            delivery->seq,
            event.handle,
            (unsigned int)event.reason,
            event.attempts_started);
        if (event.reason == NODE_COMM_TERMINAL_DELIVERED) {
            status_debug_note("DBG_MESH_TEST_DELIVERED\n");
        } else {
            if (mesh_test_drop_count < UINT32_MAX) {
                mesh_test_drop_count++;
            }
            status_debug_note("DBG_MESH_TEST_DELIVERY_FAILED\n");
            high_debug_log_event(
                "MESH_TEST_DELIVERY_TERMINAL",
                "id=%u seq=%u reason=%u attempts=%u drops=%u",
                delivery->packet_id,
                delivery->seq,
                (unsigned int)event.reason,
                event.attempts_started,
                mesh_test_drop_count);
        }
        memset(delivery, 0, sizeof(*delivery));
    }
}

static bool mesh_test_admission_retryable(int ret)
{
    return ret == -EAGAIN || ret == -EBUSY || ret == -EWOULDBLOCK ||
           ret == -EINPROGRESS || ret == -ENOSPC || ret == -ESHUTDOWN;
}

static uint64_t mesh_test_delivery_deadline_ms(void)
{
    uint64_t now_ms = (uint64_t)k_uptime_get();

    return UINT64_MAX - now_ms < MESH_TEST_DELIVERY_DEADLINE_MS ?
           UINT64_MAX : now_ms + MESH_TEST_DELIVERY_DEADLINE_MS;
}

static bool mesh_test_route_telemetry_enabled(uint64_t target_id)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) &&
           target_id == GATEWAY_ID;
}

static void mesh_test_route_start_if_needed(void)
{
    if (!mesh_test_route_start_seen) {
        mesh_test_route_start_seen = true;
        mesh_test_route_start_ms = k_uptime_get_32();
        mesh_test_route_next_summary_ms =
            mesh_test_route_start_ms + MESH_TEST_SUMMARY_INTERVAL_MS;
        mesh_test_route_start_status = 0;
    }
}

static uint32_t mesh_test_route_latency_ms(uint32_t now_ms)
{
    if (!mesh_test_route_start_seen) {
        return 0u;
    }
    if (mesh_test_route_ready_logged) {
        return mesh_test_route_ready_ms - mesh_test_route_start_ms;
    }
    return now_ms - mesh_test_route_start_ms;
}

static void mesh_test_route_summary_print(const char *phase,
                                          uint64_t target_id,
                                          uint64_t next_hop_id)
{
    uint32_t now_ms = k_uptime_get_32();

    status_debug_printf("DBG_STARTUP_ROUTE_SUMMARY phase=%s ready=%u target=0x%llx next=0x%llx\n",
                        phase == NULL ? "route" : phase,
                        mesh_test_route_ready_logged ? 1u : 0u,
                        (unsigned long long)target_id,
                        (unsigned long long)next_hop_id);
    status_debug_printf("DBG_STARTUP_ROUTE_COUNTS attempts=%u ttl=%u miss=%u prep=%u probe_ok=%u probe_fail=%u status=%d\n",
                        mesh_test_route_start_attempts,
                        mesh_test_route_start_ttl,
                        mesh_test_route_reply_misses,
                        mesh_test_route_prep_backoffs,
                        mesh_test_direct_probe_successes,
                        mesh_test_direct_probe_failures,
                        (int)mesh_test_route_start_status);
    status_debug_printf("DBG_STARTUP_ROUTE_TIMING start=%u ready_ms=%u latency=%u queued=%u now=%u\n",
                        mesh_test_route_start_ms,
                        mesh_test_route_ready_ms,
                        mesh_test_route_latency_ms(now_ms),
                        mesh_test_queued_total,
                        now_ms);
}

static void mesh_test_route_periodic_summary(void)
{
    uint32_t now_ms;

    if (!mesh_test_route_start_seen) {
        return;
    }
    if (mesh_test_route_ready_logged &&
        mesh_test_route_summary_repeats >= MESH_TEST_ROUTE_SUMMARY_REPEATS) {
        return;
    }

    now_ms = k_uptime_get_32();
    if (mesh_test_route_next_summary_ms != 0u &&
        (int32_t)(now_ms - mesh_test_route_next_summary_ms) < 0) {
        return;
    }

    mesh_test_route_summary_print(mesh_test_route_ready_logged ? "periodic" : "waiting",
                                  GATEWAY_ID,
                                  mesh_test_route_next_hop_id);
    if (mesh_test_route_ready_logged &&
        mesh_test_route_summary_repeats < UINT8_MAX) {
        mesh_test_route_summary_repeats++;
    }
    mesh_test_route_next_summary_ms = now_ms + MESH_TEST_SUMMARY_INTERVAL_MS;
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
    outbound->radio_channel = UWB_CHANNEL_MESH_PAYLOAD;
    outbound->queued_at_ms = k_uptime_get_32();
    outbound->queued_at_valid = true;
    return 0;
}

static uint32_t mesh_test_tx_once(void)
{
    struct mesh_outbound outbound;
    struct mesh_smoke_fast_tx_gate gate;
    struct mesh_smoke_fast_tx_decision decision;
    size_t service_pending;
    uint32_t packet_id;
    uint32_t handle;
    uint16_t attempt;
    uint8_t burst_target;
    uint64_t absolute_deadline_ms;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS;
    }
    mesh_test_reap_terminal_deliveries();
    mesh_test_route_periodic_summary();
    service_pending = app_node_comm_pending_delivery_count();
    gate = (struct mesh_smoke_fast_tx_gate) {
        /* Transport contention, route state, and ACK waits belong to node_comm. */
        .relay_tx_active = false,
        .route_waiting_active = false,
        .ack_wait_active = false,
        .queue_used = (uint32_t)MIN(
            service_pending, (size_t)MESH_TEST_SOURCE_DELIVERY_CAPACITY),
        .queue_depth = MESH_TEST_SOURCE_DELIVERY_CAPACITY,
        .configured_interval_ms = CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS,
        .fast_mode = IS_ENABLED(CONFIG_MESH_SMOKE_FAST_TX),
    };
    mesh_smoke_fast_tx_decide(&gate, &decision);
    if (!decision.can_queue) {
        if (mesh_test_wait_log_ticks == 0u || mesh_test_wait_log_ticks >= 10u) {
            status_debug_note("DBG_MESH_TEST_WAIT\n");
            if (decision.reason == MESH_SMOKE_FAST_DEFER_QUEUE_FULL) {
                status_debug_note("DBG_MESH_TEST_WAIT_BACKLOG\n");
            }
            if (service_pending > 0u) {
                status_debug_note("DBG_MESH_TEST_QUEUE_WAIT\n");
            }
            status_debug_printf("DBG_MESH_TEST_WAIT_STATE relay=0 route=0 headroom=%u ack=0 q=%u id=%u att=%u\n",
                                decision.queue_headroom,
                                (unsigned int)service_pending,
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
        if (mesh_test_pending_admission_valid) {
            outbound = mesh_test_pending_admission;
            packet_id = mesh_test_pending_admission_packet_id;
            attempt = mesh_test_pending_admission_attempt;
            absolute_deadline_ms = mesh_test_pending_admission_deadline_ms;
        } else {
            packet_id = mesh_test_next_packet_id;
            attempt = mesh_test_next_attempt();
            ret = mesh_test_build_packet(&outbound, packet_id, attempt);
            if (ret < 0) {
                if (mesh_test_drop_count < UINT32_MAX) {
                    mesh_test_drop_count++;
                }
                LOG_WRN("mesh-test packet build failed: id=%u attempt=%u ret=%d drops=%u",
                        packet_id, attempt, ret, mesh_test_drop_count);
                return decision.delay_ms;
            }
            absolute_deadline_ms = mesh_test_delivery_deadline_ms();
        }

        handle = 0u;
        ret = app_node_comm_submit_reliable_uplink(
            &outbound,
            absolute_deadline_ms,
            packet_id,
            &handle);
        if (ret == 0) {
            ret = mesh_test_track_delivery(handle,
                                           packet_id,
                                           outbound.packet.seq);
            if (ret < 0) {
                (void)app_node_comm_abandon_delivery(handle);
            }
        }
        if (ret == 0) {
            status_debug_note("DBG_MESH_TEST_ADMITTED\n");
            mesh_test_pending_admission_valid = false;
            mesh_test_note_queued(packet_id, outbound.packet.seq);
            mesh_test_advance_packet_id();
            mesh_test_reset_attempts();
            status_debug_printf("DBG_MESH_TEST_QUEUED id=%u att=%u q=%u burst=%u/%u\n",
                                packet_id,
                                attempt,
                                (unsigned int)app_node_comm_pending_delivery_count(),
                                (uint8_t)(queued_count + 1u),
                                burst_target);
            continue;
        }

        if (mesh_test_admission_retryable(ret)) {
            status_debug_note("DBG_MESH_TEST_ADMISSION_DEFERRED\n");
            if (!mesh_test_pending_admission_valid) {
                mesh_test_pending_admission = outbound;
                mesh_test_pending_admission_deadline_ms = absolute_deadline_ms;
                mesh_test_pending_admission_packet_id = packet_id;
                mesh_test_pending_admission_attempt = attempt;
                mesh_test_pending_admission_valid = true;
            }
            high_debug_log_event(
                "MESH_TEST_ADMISSION_DEFER",
                "id=%u attempt=%u seq=%u ret=%d pending=%u",
                packet_id,
                attempt,
                outbound.packet.seq,
                ret,
                (unsigned int)service_pending);
            return MESH_TEST_RETRY_ADMISSION_MS;
        }

        mesh_test_pending_admission_valid = false;
        status_debug_note("DBG_MESH_TEST_ADMISSION_REJECTED\n");
        if (mesh_test_drop_count < UINT32_MAX) {
            mesh_test_drop_count++;
        }
        high_debug_log_event("MESH_TEST_TX_RETRY",
                             "id=%u attempt=%u ret=%d drops=%u queued=%u",
                             packet_id,
                             attempt,
                             ret,
                             mesh_test_drop_count,
                             queued_count);
        LOG_WRN("mesh-test synthetic packet not launched: id=%u attempt=%u ret=%d drops=%u queued=%u",
                packet_id, attempt, ret, mesh_test_drop_count, queued_count);
        mesh_test_advance_packet_id();
        mesh_test_reset_attempts();
        return 250u;
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

void app_mesh_test_note_direct_gateway_route_probe(uint64_t target_id, int ret)
{
    if (!mesh_test_route_telemetry_enabled(target_id) ||
        mesh_test_route_ready_logged) {
        return;
    }

    mesh_test_route_start_if_needed();
    if (ret == 0) {
        mesh_test_inc_u16(&mesh_test_direct_probe_successes);
    } else {
        mesh_test_inc_u16(&mesh_test_direct_probe_failures);
    }
    mesh_test_route_start_status = ret;
}

void app_mesh_test_note_route_request_attempt(uint64_t target_id,
                                              uint8_t attempt_count,
                                              uint8_t ttl)
{
    if (!mesh_test_route_telemetry_enabled(target_id) ||
        mesh_test_route_ready_logged) {
        return;
    }

    mesh_test_route_start_if_needed();
    mesh_test_route_start_attempts = attempt_count;
    mesh_test_route_start_ttl = ttl;
    mesh_test_route_start_status = 0;
}

void app_mesh_test_note_route_request_prepare_result(uint64_t target_id, int ret)
{
    if (!mesh_test_route_telemetry_enabled(target_id) ||
        mesh_test_route_ready_logged ||
        ret == PROTO_OK) {
        return;
    }

    mesh_test_route_start_if_needed();
    if (ret == PROTO_ERR_BUSY) {
        mesh_test_inc_u16(&mesh_test_route_prep_backoffs);
    }
    mesh_test_route_start_status = ret;
}

void app_mesh_test_note_route_reply_miss(uint64_t target_id, int ret)
{
    if (!mesh_test_route_telemetry_enabled(target_id) ||
        mesh_test_route_ready_logged) {
        return;
    }

    mesh_test_route_start_if_needed();
    mesh_test_inc_u16(&mesh_test_route_reply_misses);
    mesh_test_route_start_status = ret;
}

void app_mesh_test_note_route_ready(uint64_t target_id,
                                    uint64_t next_hop_id,
                                    int status)
{
    if (!mesh_test_route_telemetry_enabled(target_id)) {
        return;
    }

    mesh_test_route_start_if_needed();
    mesh_test_route_next_hop_id = next_hop_id;
    mesh_test_route_ready_ms = k_uptime_get_32();
    mesh_test_route_start_status = status;
    mesh_test_route_ready_logged = true;
    mesh_test_route_summary_repeats = 0u;
    mesh_test_route_next_summary_ms =
        mesh_test_route_ready_ms + MESH_TEST_SUMMARY_INTERVAL_MS;
    mesh_test_route_summary_print("ready", target_id, next_hop_id);
}

void app_mesh_test_note_report_tx_retryable(uint16_t seq, int ret)
{
    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ||
        mesh_test_first_ack_logged) {
        return;
    }
    mesh_test_retryable_before_first_ack++;
    status_debug_printf("DBG_STARTUP_TX_RETRY seq=%u ret=%d retries=%u uptime=%u\n",
                        seq,
                        ret,
                        mesh_test_retryable_before_first_ack,
                        k_uptime_get_32());
}

void app_mesh_test_note_report_tx_backoff(uint16_t seq, int ret, uint32_t delay_ms)
{
    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ||
        mesh_test_first_ack_logged) {
        return;
    }
    mesh_test_backoff_before_first_ack++;
    status_debug_printf("DBG_STARTUP_TX_BACKOFF seq=%u ret=%d backoffs=%u delay=%u uptime=%u\n",
                        seq,
                        ret,
                        mesh_test_backoff_before_first_ack,
                        delay_ms,
                        k_uptime_get_32());
}

void app_mesh_test_note_direct_gateway_ack(uint16_t seq, int ret, uint32_t queue_depth)
{
    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ||
        mesh_test_first_ack_logged) {
        return;
    }
    if (ret != 0) {
        mesh_test_ack_fail_before_first_success++;
        return;
    }
    mesh_test_first_ack_logged = true;
    status_debug_printf("DBG_STARTUP_FIRST_ACK seq=%u uptime=%u first_q_ms=%u first_id=%u first_seq=%u queued=%u retryable=%u backoff=%u ack_fail=%u q=%u\n",
                        seq,
                        k_uptime_get_32(),
                        mesh_test_first_queue_ms,
                        mesh_test_first_queue_id,
                        mesh_test_first_queue_seq,
                        mesh_test_queued_total,
                        mesh_test_retryable_before_first_ack,
                        mesh_test_backoff_before_first_ack,
                        mesh_test_ack_fail_before_first_success,
                        queue_depth);
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

void app_mesh_test_note_direct_gateway_route_probe(uint64_t target_id, int ret)
{
    ARG_UNUSED(target_id);
    ARG_UNUSED(ret);
}

void app_mesh_test_note_route_request_attempt(uint64_t target_id,
                                              uint8_t attempt_count,
                                              uint8_t ttl)
{
    ARG_UNUSED(target_id);
    ARG_UNUSED(attempt_count);
    ARG_UNUSED(ttl);
}

void app_mesh_test_note_route_request_prepare_result(uint64_t target_id, int ret)
{
    ARG_UNUSED(target_id);
    ARG_UNUSED(ret);
}

void app_mesh_test_note_route_reply_miss(uint64_t target_id, int ret)
{
    ARG_UNUSED(target_id);
    ARG_UNUSED(ret);
}

void app_mesh_test_note_route_ready(uint64_t target_id,
                                    uint64_t next_hop_id,
                                    int status)
{
    ARG_UNUSED(target_id);
    ARG_UNUSED(next_hop_id);
    ARG_UNUSED(status);
}

void app_mesh_test_note_report_tx_retryable(uint16_t seq, int ret)
{
    ARG_UNUSED(seq);
    ARG_UNUSED(ret);
}

void app_mesh_test_note_report_tx_backoff(uint16_t seq, int ret, uint32_t delay_ms)
{
    ARG_UNUSED(seq);
    ARG_UNUSED(ret);
    ARG_UNUSED(delay_ms);
}

void app_mesh_test_note_direct_gateway_ack(uint16_t seq, int ret, uint32_t queue_depth)
{
    ARG_UNUSED(seq);
    ARG_UNUSED(ret);
    ARG_UNUSED(queue_depth);
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
