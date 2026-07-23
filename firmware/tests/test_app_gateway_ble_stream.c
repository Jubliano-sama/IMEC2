#include "app_gateway_ble_stream.h"
#include "app_gateway_command_observability.h"

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

static void test_max_size_click_is_preserved_and_oversize_is_rejected(void)
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
                                             GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN,
                                             0u,
                                             1u,
                                             true) == 1);
    assert(gateway_ble_stream_peek(&state, &record, &record_len) == 0);
    assert(record_len == GATEWAY_BLE_STREAM_RECORD_MAX_LEN);
    assert((record[7] & 0x01u) == 0u);

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

static void test_click_cannot_evict_durable_result_records(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct send_capture capture = {0};
    uint8_t payload[1] = {0u};
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 1u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 20u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        result.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             20u,
                                             true) == -ENOSPC);
    assert(!state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    gateway_ble_stream_get_diagnostics(&state, 20u, &diag);
    assert(diag.drops_priority == 0u);
    assert(diag.drops_queue_full == 1u);
    assert(gateway_ble_stream_drain(&state,
                                    capture_send,
                                    &capture,
                                    30u,
                                    true,
                                    GATEWAY_BLE_STREAM_QUEUE_DEPTH) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        assert(capture.msg_types[i] == MSG_COMMAND_RESULT);
    }
}

static void test_retained_assignment_event_survives_click_eviction(void)
{
    struct gateway_ble_stream_state state;
    struct proto_packet publisher = packet(MSG_GATEWAY_COMMAND_EVENT, 0u, 10u);
    struct proto_packet diagnostic = packet(MSG_UWB_ANCHOR_DIAG, 0u, 20u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 30u);
    struct proto_packet head;
    uint8_t payload[1] = {0u};

    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state, &publisher,
                                             payload, sizeof(payload),
                                             0u, 1u, true) == 1);
    state.items[state.count - 1u].retain_until_sent = true;
    assert(gateway_ble_stream_enqueue_packet(&state, &diagnostic,
                                             payload, sizeof(payload),
                                             0u, 2u, true) == 1);
    diagnostic.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state, &diagnostic,
                                             payload, sizeof(payload),
                                             0u, 3u, true) == 1);

    assert(gateway_ble_stream_enqueue_packet(&state, &click,
                                             payload, sizeof(payload),
                                             0u, 4u, true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state, &click,
                                             payload, sizeof(payload),
                                             0u, 5u, true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state, &click,
                                             payload, sizeof(payload),
                                             0u, 6u, true) == -ENOSPC);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(gateway_ble_stream_head_packet(&state, &head) == 0);
    assert(head.msg_type == MSG_GATEWAY_COMMAND_EVENT);
    assert(head.seq == publisher.seq);
    assert(state.items[0].retain_until_sent);
}

static void test_terminal_command_event_evicts_lower_priority_status(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_command_observability_state observability;
    struct gateway_command_event event = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_COMPLETE,
        .status = COMMAND_TIMEOUT,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NO_ANCHORS,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    struct proto_packet heartbeat = packet(MSG_ANCHOR_HEARTBEAT, 0u, 1u);
    struct proto_packet command_event = packet(MSG_GATEWAY_COMMAND_EVENT, 0u, 2u);
    struct gateway_ble_stream_diagnostics diag;
    uint8_t status_payload[1] = {0u};
    uint8_t event_payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t event_payload_len = 0u;

    gateway_ble_stream_init(&state);
    gateway_command_observability_init(&observability);
    assert(gateway_command_observability_prepare(&observability, &event, true) == 0);
    assert(gateway_command_event_encode(&event,
                                        event_payload,
                                        sizeof(event_payload),
                                        &event_payload_len) == 0);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        heartbeat.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &heartbeat,
                                                 status_payload,
                                                 sizeof(status_payload),
                                                 0u,
                                                 i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &command_event,
                                             event_payload,
                                             event_payload_len,
                                             0u,
                                             20u,
                                             true) == 1);
    gateway_ble_stream_get_diagnostics(&state, 20u, &diag);
    assert(diag.drops_priority == 1u);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
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

static void test_active_head_cannot_be_evicted(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct send_capture capture = {0};
    uint8_t active_record[GATEWAY_BLE_STREAM_RECORD_MAX_LEN];
    uint8_t payload[1] = {0u};
    size_t active_len = 0u;
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
    assert(gateway_ble_stream_begin_send(&state,
                                         active_record,
                                         sizeof(active_record),
                                         &active_len) == 0);
    assert(active_len > GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    assert(active_record[8] == MSG_UWB_ANCHOR_DIAG);
    assert(gateway_ble_stream_begin_send(&state,
                                         active_record,
                                         sizeof(active_record),
                                         &active_len) == -EBUSY);

    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             20u,
                                             true) == 1);
    assert(active_record[8] == MSG_UWB_ANCHOR_DIAG);
    gateway_ble_stream_get_diagnostics(&state, 20u, &diag);
    assert(diag.drops_priority == 1u);

    gateway_ble_stream_mark_sent(&state, 21u);
    assert(gateway_ble_stream_drain(&state,
                                    capture_send,
                                    &capture,
                                    30u,
                                    true,
                                    GATEWAY_BLE_STREAM_QUEUE_DEPTH) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u);
    assert(capture.msg_types[GATEWAY_BLE_STREAM_QUEUE_DEPTH - 2u] ==
           MSG_CLICK_REPORT);

    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &diagnostic,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             1u,
                                             true) == 1);
    assert(gateway_ble_stream_begin_send(&state,
                                         active_record,
                                         sizeof(active_record),
                                         &active_len) == 0);
    gateway_ble_stream_cancel_send(&state);
    assert(gateway_ble_stream_begin_send(&state,
                                         active_record,
                                         sizeof(active_record),
                                         &active_len) == 0);
}

static void test_pool_holds_core_click_and_two_cir_records(void)
{
    struct gateway_ble_stream_state state;
    uint8_t payload[GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN];
    struct proto_packet click = packet(MSG_CLICK_REPORT,
                                       FLAG_DIAGNOSTIC,
                                       1u);

    fill_payload(payload, sizeof(payload));
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             PACKET_MAX_PAYLOAD_LEN,
                                             0u,
                                             1u,
                                             true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             PACKET_EXT_MAX_PAYLOAD_LEN,
                                             0u,
                                             2u,
                                             true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             GATEWAY_BLE_STREAM_CLICK_CIR_TAIL_PAYLOAD_BYTES,
                                             0u,
                                             3u,
                                             true) == 1);
    assert(gateway_ble_stream_depth(&state) == 3u);
    assert(state.pool_used == GATEWAY_BLE_STREAM_CLICK_CIR_BURST_BYTES);
    assert(state.pool_used <= GATEWAY_BLE_STREAM_RECORD_POOL_BYTES);
}

static void test_pool_boundary_preserves_retained_click_custody(void)
{
    struct gateway_ble_stream_state state;
    uint8_t payload[GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN];
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 1u);
    struct proto_packet diagnostic =
        packet(MSG_UWB_ANCHOR_DIAG_FRAGMENT, 0u, 2u);
    struct proto_packet status = packet(MSG_ANCHOR_HEARTBEAT, 0u, 3u);
    uint16_t pool_before;

    fill_payload(payload, sizeof(payload));

    /* The documented click/CIR burst occupies every queue slot and must
     * remain admissible after the pool was reduced to the deployable budget. */
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             PACKET_MAX_PAYLOAD_LEN,
                                             0u,
                                             1u,
                                             true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             PACKET_EXT_MAX_PAYLOAD_LEN,
                                             0u,
                                             2u,
                                             true) == 1);
    click.seq++;
    assert(gateway_ble_stream_enqueue_packet(
               &state,
               &click,
               payload,
               GATEWAY_BLE_STREAM_CLICK_CIR_TAIL_PAYLOAD_BYTES,
               0u,
               3u,
               true) == 1);
    assert(state.pool_used == GATEWAY_BLE_STREAM_CLICK_CIR_BURST_BYTES);
    assert(state.pool_used + GATEWAY_BLE_STREAM_RECORD_POOL_SAFETY_MARGIN_BYTES <=
           GATEWAY_BLE_STREAM_RECORD_POOL_BYTES);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        state.items[i].retain_until_sent = true;
    }
    pool_before = state.pool_used;
    click.seq = 99u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             1u,
                                             0u,
                                             4u,
                                             true) == -ENOSPC);
    assert(state.pool_used == pool_before);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(state.items[0].packet.msg_type == MSG_CLICK_REPORT);
    assert(state.items[0].packet.seq == 1u);
    assert(state.items[0].retain_until_sent);

    /* Fill the 1756-byte pool exactly with three retained records, then
     * prove that a record requiring one more byte cannot displace custody. */
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(
               &state,
               &click,
               payload,
               GATEWAY_BLE_STREAM_RECORD_POOL_BYTES -
                   (3u * GATEWAY_BLE_STREAM_RECORD_HEADER_LEN) -
                   PACKET_EXT_MAX_PAYLOAD_LEN,
               0u,
               10u,
               true) == 1);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &diagnostic,
                                             payload,
                                             PACKET_EXT_MAX_PAYLOAD_LEN,
                                             0u,
                                             11u,
                                             true) == 1);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &status,
                                             payload,
                                             0u,
                                             0u,
                                             12u,
                                             true) == 1);
    assert(state.pool_used == GATEWAY_BLE_STREAM_RECORD_POOL_BYTES);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        state.items[i].retain_until_sent = true;
    }
    pool_before = state.pool_used;
    click.seq = 100u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             1u,
                                             0u,
                                             13u,
                                             true) == -ENOSPC);
    assert(state.pool_used == pool_before);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(state.items[0].packet.msg_type == MSG_CLICK_REPORT);

    /* With one queue slot still available, make the attempted record exceed
     * the remaining pool by exactly one byte, isolating the byte-capacity
     * guard from queue-depth backpressure. */
    gateway_ble_stream_init(&state);
    click.seq = 200u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             677u,
                                             0u,
                                             20u,
                                             true) == 1);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &diagnostic,
                                             payload,
                                             PACKET_EXT_MAX_PAYLOAD_LEN,
                                             0u,
                                             21u,
                                             true) == 1);
    state.items[0].retain_until_sent = true;
    state.items[1].retain_until_sent = true;
    pool_before = state.pool_used;
    click.seq = 201u;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &click,
                                             payload,
                                             2u,
                                             0u,
                                             22u,
                                             true) == -ENOSPC);
    assert(pool_before + GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + 2u ==
           GATEWAY_BLE_STREAM_RECORD_POOL_BYTES + 1u);
    assert(state.pool_used == pool_before);
    assert(gateway_ble_stream_depth(&state) == 2u);
    assert(state.items[0].packet.seq == 200u);
    assert(state.items[0].retain_until_sent);
}

static void test_staged_extended_payload_survives_overlap_and_pressure(void)
{
    struct gateway_ble_stream_state state;
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 401u);
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 500u);
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    uint8_t result_payload[1] = {0x5au};
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    size_t staging_offset =
        GATEWAY_BLE_STREAM_RECORD_POOL_BYTES - PACKET_EXT_MAX_PAYLOAD_LEN;
    uint16_t pool_before;

    fill_payload(payload, sizeof(payload));
    click.payload_len = sizeof(payload);

    /* The 958-byte source tail overlaps the first destination record. */
    gateway_ble_stream_init(&state);
    memcpy(&state.record_pool[staging_offset], payload, sizeof(payload));
    state.restore_staging_active = true;
    state.restore_staging_offset = (uint16_t)staging_offset;
    assert(gateway_ble_stream_enqueue_staged_packet(&state,
                                                    &click,
                                                    sizeof(payload),
                                                    0u,
                                                    1u,
                                                    true) == 1);
    state.restore_staging_active = false;
    state.restore_staging_offset = 0u;
    assert(gateway_ble_stream_peek(&state, &record, &record_len) == 0);
    assert(record_len == GATEWAY_BLE_STREAM_RECORD_HEADER_LEN +
                              sizeof(payload));
    assert(memcmp(&record[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN],
                  payload,
                  sizeof(payload)) == 0);

    /* Full BLE pressure must reject a staged click without changing the
     * retained queue or the bytes that a later retry will restore. */
    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        result.seq = (uint16_t)(500u + i);
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &result,
                                                 result_payload,
                                                 sizeof(result_payload),
                                                 0u,
                                                 (uint32_t)(10u + i),
                                                 true) == 1);
    }
    pool_before = state.pool_used;
    memcpy(&state.record_pool[staging_offset], payload, sizeof(payload));
    state.restore_staging_active = true;
    state.restore_staging_offset = (uint16_t)staging_offset;
    assert(gateway_ble_stream_enqueue_staged_packet(&state,
                                                    &click,
                                                    sizeof(payload),
                                                    0u,
                                                    20u,
                                                    true) == -ENOSPC);
    assert(state.pool_used == pool_before);
    assert(gateway_ble_stream_depth(&state) == GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(memcmp(&state.record_pool[staging_offset],
                  payload,
                  sizeof(payload)) == 0);
    state.restore_staging_active = false;
    state.restore_staging_offset = 0u;
}

static void test_reservation_reports_full_queue_backpressure(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 1u);
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 20u);
    uint8_t payload[1] = {0x5au};

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        click.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &click,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             10u,
                                             true) == -ENOSPC);
    assert(!state.reservation_active);
    assert(gateway_ble_stream_depth(&state) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    gateway_ble_stream_get_diagnostics(&state, 10u, &diag);
    assert(diag.drops_queue_full == 1u);

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        click.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &click,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 false) == 1);
    }
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             10u,
                                             false) == -ENOTCONN);
    gateway_ble_stream_get_diagnostics(&state, 10u, &diag);
    assert(diag.drops_not_ready == 1u);
}

static void test_reservation_protects_capacity_and_cancel_releases_it(void)
{
    struct gateway_ble_stream_state state;
    struct proto_packet reserved = packet(MSG_CLICK_REPORT, 0u, 1u);
    struct proto_packet other = packet(MSG_CLICK_REPORT, 0u, 2u);
    uint8_t payload[GATEWAY_BLE_STREAM_PAYLOAD_MAX_LEN];

    fill_payload(payload, sizeof(payload));
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_reserve_packet(
               &state,
               &reserved,
               payload,
               sizeof(payload),
               0u,
               1u,
               true) == 1);
    assert(state.reservation_active);
    assert(state.pool_used == 0u);
    assert(gateway_ble_stream_depth(&state) == 0u);
    assert(state.items[GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u].len ==
           GATEWAY_BLE_STREAM_RECORD_MAX_LEN);
    assert(state.reservation_payload_len == sizeof(payload));
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             2u,
                                             true) == -EBUSY);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             2u,
                                             true) == -ENOSPC);
    assert(state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == 0u);

    gateway_ble_stream_cancel_reservation(&state);
    assert(!state.reservation_active);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             2u,
                                             true) == 1);
}

static void test_reservation_commit_is_exact_and_single_use(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_state expected;
    struct gateway_ble_stream_diagnostics diag;
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 41u);
    struct proto_packet wrong;
    const uint8_t *record = NULL;
    const uint8_t *expected_record = NULL;
    uint8_t payload[7] = {1u, 3u, 5u, 7u, 9u, 11u, 13u};
    uint8_t wrong_payload[sizeof(payload)];
    size_t record_len = 0u;
    size_t expected_len = 0u;

    result.payload_len = sizeof(payload);
    gateway_ble_stream_init(&state);
    gateway_ble_stream_init(&expected);
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             123u,
                                             true) == 1);

    wrong = result;
    wrong.seq++;
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &wrong,
                                                 payload,
                                                 sizeof(payload)) == -ESTALE);
    memcpy(wrong_payload, payload, sizeof(payload));
    wrong_payload[3] ^= 0x80u;
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 wrong_payload,
                                                 sizeof(wrong_payload)) ==
           -ESTALE);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload) - 1u) ==
           -ESTALE);
    assert(state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == 0u);

    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload)) == 1);
    assert(!state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == 1u);
    assert(gateway_ble_stream_enqueue_packet(&expected,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             123u,
                                             true) == 1);
    assert(gateway_ble_stream_peek(&state, &record, &record_len) == 0);
    assert(gateway_ble_stream_peek(&expected,
                                   &expected_record,
                                   &expected_len) == 0);
    assert(record_len == expected_len);
    assert(memcmp(record, expected_record, record_len) == 0);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload)) == -ENOENT);
    assert(gateway_ble_stream_depth(&state) == 1u);
    gateway_ble_stream_get_diagnostics(&state, 124u, &diag);
    assert(diag.enqueue_attempts == 1u);
}

static void test_reservation_uses_priority_eviction_and_survives_drain(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    struct send_capture capture = {0};
    struct proto_packet status = packet(MSG_ANCHOR_HEARTBEAT, 0u, 1u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 50u);
    struct proto_packet other = packet(MSG_ANCHOR_HEARTBEAT, 0u, 60u);
    uint8_t payload[1] = {0x33u};

    gateway_ble_stream_init(&state);
    for (uint8_t i = 0u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        status.seq = i;
        assert(gateway_ble_stream_enqueue_packet(&state,
                                                 &status,
                                                 payload,
                                                 sizeof(payload),
                                                 0u,
                                                 i,
                                                 true) == 1);
    }
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &click,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             10u,
                                             true) == 1);
    assert(state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == 2u);
    gateway_ble_stream_get_diagnostics(&state, 10u, &diag);
    assert(diag.drops_priority == 1u);

    assert(gateway_ble_stream_drain(&state,
                                    capture_send,
                                    &capture,
                                    11u,
                                    true,
                                    1u) == 1u);
    assert(state.reservation_active);
    assert(gateway_ble_stream_depth(&state) == 1u);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             12u,
                                             true) == 1);
    other.seq++;
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             0u,
                                             13u,
                                             true) == -ENOSPC);
    assert(state.reservation_active);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &click,
                                                 payload,
                                                 sizeof(payload)) == 1);
    assert(gateway_ble_stream_depth(&state) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH);
}

static void test_ble_recovery_backoff_is_random_exponential_and_capped(void)
{
    assert(gateway_ble_recovery_backoff_ms(0u, 0u) == 250u);
    assert(gateway_ble_recovery_backoff_ms(0u, 249u) == 499u);
    assert(gateway_ble_recovery_backoff_ms(1u, 0u) == 500u);
    assert(gateway_ble_recovery_backoff_ms(2u, 799u) == 1799u);
    assert(gateway_ble_recovery_backoff_ms(UINT8_MAX, 0u) == 30000u);
    assert(gateway_ble_recovery_backoff_ms(UINT8_MAX, UINT32_MAX) == 30000u);
}

int main(void)
{
    test_allowlist_excludes_mesh_control();
    test_queue_full_counts_drop();
    test_max_size_click_is_preserved_and_oversize_is_rejected();
    test_disconnected_full_queue_counts_not_ready();
    test_click_evicts_lower_priority_diagnostic();
    test_click_cannot_evict_durable_result_records();
    test_retained_assignment_event_survives_click_eviction();
    test_terminal_command_event_evicts_lower_priority_status();
    test_fast_drain_and_counters();
    test_active_head_cannot_be_evicted();
    test_pool_holds_core_click_and_two_cir_records();
    test_pool_boundary_preserves_retained_click_custody();
    test_staged_extended_payload_survives_overlap_and_pressure();
    test_reservation_reports_full_queue_backpressure();
    test_reservation_protects_capacity_and_cancel_releases_it();
    test_reservation_commit_is_exact_and_single_use();
    test_reservation_uses_priority_eviction_and_survives_drain();
    test_ble_recovery_backoff_is_random_exponential_and_capped();
    return 0;
}
