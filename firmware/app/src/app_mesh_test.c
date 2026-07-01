#include "app_mesh_test.h"

#include "app_config.h"
#include "app_gateway_ble.h"
#include "app_high_debug.h"
#include "app_mesh_report.h"
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

#ifndef CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS
#define CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS 1000
#endif

K_THREAD_STACK_DEFINE(mesh_test_thread_stack, MESH_TEST_WORKQUEUE_STACK_SIZE);
static struct k_thread mesh_test_thread;
static bool mesh_test_thread_started;
static uint32_t mesh_test_next_packet_id = 1u;
static uint16_t mesh_test_attempt;
static uint16_t mesh_test_seq;
static uint32_t mesh_test_drop_count;
static bool mesh_test_continuous_ch5_active;
static uint8_t mesh_test_wait_log_ticks;

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

static int mesh_test_append_payload(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *payload_len,
                                    uint32_t packet_id,
                                    uint16_t attempt)
{
    const uint32_t flags = MESH_TEST_FLAG_GATEWAY_ACK_REQUIRED |
                           MESH_TEST_FLAG_CHANNEL9_PAYLOAD |
                           MESH_TEST_FLAG_CH5_WAKE_CONTINUOUS;
    int ret;

    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_PACKET_ID, packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_ATTEMPT, attempt);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_DROP_COUNT, mesh_test_drop_count);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_ORIGIN_ID, DEVICE_ID);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u64(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_TARGET_ID, GATEWAY_ID);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_MESH_TEST_FLAGS, flags);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_EVENT_SEQ, packet_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, payload_len,
                        TLV_RETRY_COUNT, (uint8_t)MIN(attempt, (uint16_t)UINT8_MAX));
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u32(payload, payload_cap, payload_len,
                         TLV_UPTIME_MS, k_uptime_get_32());
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u8(payload, payload_cap, payload_len,
                        TLV_DEVICE_ROLE, (uint8_t)DEVICE_ROLE);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, payload_len,
                         TLV_MESH_CHANNEL, UWB_CHANNEL_MESH_PAYLOAD);
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
    outbound->packet.payload_len = (uint8_t)payload_len;
    outbound->payload_len = (uint8_t)payload_len;
    return 0;
}

static uint32_t mesh_test_tx_once(void)
{
    struct mesh_outbound outbound;
    uint32_t packet_id;
    uint16_t attempt;
    bool relay_tx_active;
    bool route_waiting_active;
    bool report_backlog_active;
    bool ack_wait_active;
    int ret;

    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS;
    }
    relay_tx_active = mesh_relay_tx_active(&mesh_runtime);
    route_waiting_active = mesh_route_waiting_tx_active();
    report_backlog_active = mesh_report_tx_backlog_active();
    ack_wait_active = mesh_report_ch9_ack_wait_active();
    if (relay_tx_active || route_waiting_active || report_backlog_active ||
        ack_wait_active) {
        status_debug_note("DBG_MESH_TEST_WAIT\n");
        if (relay_tx_active) {
            status_debug_note("DBG_MESH_TEST_WAIT_RELAY\n");
        }
        if (route_waiting_active) {
            status_debug_note("DBG_MESH_TEST_WAIT_ROUTE\n");
        }
        if (report_backlog_active) {
            status_debug_note("DBG_MESH_TEST_WAIT_BACKLOG\n");
        }
        if (ack_wait_active) {
            status_debug_note("DBG_MESH_TEST_WAIT_ACK\n");
        }
        if (report_tx_queue_used() > 0u) {
            status_debug_note("DBG_MESH_TEST_QUEUE_WAIT\n");
        }
        if (mesh_test_wait_log_ticks == 0u || mesh_test_wait_log_ticks >= 10u) {
            status_debug_printf("DBG_MESH_TEST_WAIT_STATE relay=%u route=%u backlog=%u ack=%u q=%u id=%u att=%u\n",
                                relay_tx_active ? 1u : 0u,
                                route_waiting_active ? 1u : 0u,
                                report_backlog_active ? 1u : 0u,
                                ack_wait_active ? 1u : 0u,
                                report_tx_queue_used(),
                                mesh_test_next_packet_id,
                                mesh_test_attempt);
            mesh_test_wait_log_ticks = 1u;
        } else {
            mesh_test_wait_log_ticks++;
        }
        return 100u;
    }
    mesh_test_wait_log_ticks = 0u;

    packet_id = mesh_test_next_packet_id;
    attempt = mesh_test_next_attempt();
    ret = mesh_test_build_packet(&outbound, packet_id, attempt);
    if (ret < 0) {
        mesh_test_drop_count++;
        LOG_WRN("mesh-test packet build failed: id=%u attempt=%u ret=%d drops=%u",
                packet_id, attempt, ret, mesh_test_drop_count);
        return CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS;
    }

    ret = queue_anchor_report(&outbound);
    status_debug_note(ret == 0 ? "DBG_MESH_TEST_SEND_OK\n" :
                      "DBG_MESH_TEST_SEND_FAIL\n");
    if (ret == 0) {
        mesh_test_next_packet_id++;
        if (mesh_test_next_packet_id == 0u) {
            mesh_test_next_packet_id = 1u;
        }
        mesh_test_reset_attempts();
        status_debug_printf("DBG_MESH_TEST_QUEUED id=%u att=%u q=%u\n",
                            packet_id,
                            attempt,
                            report_tx_queue_used());
        return CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS;
    }

    mesh_test_drop_count++;
    high_debug_log_event("MESH_TEST_TX_RETRY",
                         "id=%u attempt=%u ret=%d drops=%u",
                         packet_id,
                         attempt,
                         ret,
                         mesh_test_drop_count);
    LOG_WRN("mesh-test synthetic packet not launched: id=%u attempt=%u ret=%d drops=%u",
            packet_id, attempt, ret, mesh_test_drop_count);
    return ret == -EBUSY ? 100u : 250u;
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
    LOG_INF("mesh-test synthetic transmitter enabled: dst=0x%016llx interval_ms=%u startup_delay_ms=%u",
            (unsigned long long)GATEWAY_ID,
            CONFIG_IMEC_MESH_ROUTE_TEST_TX_INTERVAL_MS,
            CONFIG_IMEC_MESH_ROUTE_TEST_STARTUP_GRACE_MS);
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

    target_scan_interval_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ?
                              CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS :
                              0u;
    if (anchor_uwb_scan_interval_ms != target_scan_interval_ms) {
        anchor_uwb_scan_interval_ms = target_scan_interval_ms;
    }
    if (!mesh_test_continuous_ch5_active) {
        mesh_test_continuous_ch5_active = true;
        LOG_INF("mesh-test wake event switched anchor to continuous channel-5 scan: msg=0x%02x src=0x%016llx dst=0x%016llx prev=0x%016llx quality=%u interval_ms=%u",
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

    target_scan_interval_ms = IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER) ?
                              CONFIG_IMEC_MESH_ROUTE_TEST_CH5_SCAN_INTERVAL_MS :
                              0u;
    if (anchor_uwb_scan_interval_ms != target_scan_interval_ms) {
        anchor_uwb_scan_interval_ms = target_scan_interval_ms;
    }
    if (!mesh_test_continuous_ch5_active) {
        mesh_test_continuous_ch5_active = true;
        LOG_INF("mesh-test wake claim switched anchor to continuous channel-5 scan: src=0x%016llx event_seq=%u attempt=%u quality=%u interval_ms=%u",
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
#endif
