#include "app_gateway_ble_stream.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

struct send_capture {
    unsigned int calls;
    unsigned int fail_after;
    size_t bytes;
    uint8_t msg_types[16];
};

static struct proto_packet packet(uint8_t msg_type, uint8_t flags, uint16_t seq)
{
    struct proto_packet value = {
        .msg_type = msg_type,
        .flags = flags,
        .src_id = 0x1111222233334444ull,
        .dst_id = 0x9999888877776666ull,
        .session_id = 0x12345678u,
        .seq = seq,
        .ttl = 1u,
    };

    return value;
}

static int capture_send(const uint8_t *record, size_t record_len, void *ctx)
{
    struct send_capture *capture = ctx;

    assert(record != NULL);
    assert(record_len >= GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    if (capture->fail_after != 0u && capture->calls >= capture->fail_after) {
        return -EAGAIN;
    }
    capture->msg_types[capture->calls] = record[8];
    capture->calls++;
    capture->bytes += record_len;
    return 0;
}

static void fill_payload(uint8_t *payload, size_t payload_len)
{
    for (size_t i = 0u; i < payload_len; i++) {
        payload[i] = (uint8_t)i;
    }
}

static void test_allowlist_excludes_mesh_control(void)
{
    assert(gateway_ble_should_stream_packet(MSG_CLICK_REPORT, 0u,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_UWB_REPORT, FLAG_COUNT_AS_CLICK,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_COMMAND_RESULT, 0u,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_SURVEY_PAIR_RESULT, 0u,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_MESH_DATA, FLAG_DIAGNOSTIC,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(!gateway_ble_should_stream_packet(MSG_MESH_DATA, 0u,
                                             GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(!gateway_ble_should_stream_packet(MSG_ROUTE_REQ, 0u,
                                             GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(!gateway_ble_should_stream_packet(MSG_GATEWAY_ACK, 0u,
                                             GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
}

static void test_queue_full_counts_drop(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 1u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        click.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &click,
                                                 payload,
                                                 sizeof(payload),
                                                 100u,
                                                 110u + i,
                                                 true) == 1);
    }
    click.seq = 100u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             200u,
                                             true) == -ENOSPC);
    gateway_ble_stream_get_diagnostics(&state, 200u, &diag);
    assert(diag.enqueue_attempts == GATEWAY_BLE_STREAM_QUEUE_DEPTH + 1u);
    assert(diag.drops_queue_full == 1u);
    assert(diag.last_dropped_packet_type == MSG_CLICK_REPORT);
    assert(diag.last_drop_reason == GATEWAY_BLE_STREAM_DROP_QUEUE_FULL);
    assert(diag.max_queue_depth_observed == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
}

static void test_oversize_click_is_rejected_and_diagnostic_is_truncated(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    uint8_t payload[GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN + 4u];
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 1u);
    struct proto_packet diagnostic = packet(MSG_UWB_ANCHOR_DIAG_FRAGMENT, 0u, 2u);
    const uint8_t *record = NULL;
    size_t record_len = 0u;

    fill_payload(payload, sizeof(payload));
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             1u,
                                             true) == -EMSGSIZE);
    gateway_ble_stream_get_diagnostics(&state, 1u, &diag);
    assert(diag.drops_too_large == 1u);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &diagnostic,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             2u,
                                             true) == 1);
    assert(gateway_ble_stream_peek(&state, &record, &record_len) == 0);
    assert(record_len == GATEWAY_BLE_STREAM_RECORD_MAX_LEN);
    assert((record[7] & 0x01u) != 0u);
}

static void test_disconnected_full_queue_counts_not_ready(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    uint8_t payload[1] = {0u};
    struct proto_packet heartbeat = packet(MSG_ANCHOR_HEARTBEAT, 0u, 1u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        heartbeat.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &heartbeat,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 false) == 1);
    }
    heartbeat.seq = 100u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &heartbeat,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             200u,
                                             false) == -ENOTCONN);
    gateway_ble_stream_get_diagnostics(&state, 200u, &diag);
    assert(diag.drops_not_ready == 1u);
    assert(diag.last_drop_reason == GATEWAY_BLE_STREAM_DROP_NOT_READY);
}

static void test_click_evicts_lower_priority_diagnostic(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct send_capture capture = {0};
    uint8_t payload[1] = {0u};
    struct proto_packet diagnostic = packet(MSG_UWB_ANCHOR_DIAG, 0u, 1u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 2u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        diagnostic.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &diagnostic,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             20u,
                                             true) == 1);
    gateway_ble_stream_get_diagnostics(&state, 20u, &diag);
    assert(diag.drops_priority == 1u);
    assert(gateway_ble_stream_drain(&state,
                                    capture_send,
                                    &capture,
                                    30u,
                                    true,
                                    GATEWAY_BLE_STREAM_QUEUE_DEPTH) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(capture.msg_types[GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u] ==
           MSG_CLICK_REPORT);
}

static void test_fast_drain_and_counters(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct send_capture capture = {0};
    uint8_t payload[3] = {1u, 2u, 3u};
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 1u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < 3u; i++) {
        result.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload),
                                                 100u,
                                                 110u + i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_drain(&state,
                                    capture_send,
                                    &capture,
                                    200u,
                                    true,
                                    8u) == 3u);
    gateway_ble_stream_get_diagnostics(&state, 200u, &diag);
    assert(capture.calls == 3u);
    assert(diag.packets_sent == 3u);
    assert(diag.bytes_sent == capture.bytes);
    assert(diag.oldest_queued_age_ms == 0u);
    assert(gateway_ble_stream_depth(&state) == 0u);
}

int main(void)
{
    test_allowlist_excludes_mesh_control();
    test_queue_full_counts_drop();
    test_oversize_click_is_rejected_and_diagnostic_is_truncated();
    test_disconnected_full_queue_counts_not_ready();
    test_click_evicts_lower_priority_diagnostic();
    test_fast_drain_and_counters();
    return 0;
}
