#include "app_gateway_ble_stream.h"
#include "app_gateway_command_observability.h"
#include "gateway_command.h"

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

static void make_crc16_collision(uint8_t first[4], uint8_t second[4])
{
    uint16_t target;

    memset(first, 0, 4u);
    memset(second, 0, 4u);
    second[0] = 1u;
    second[1] = 2u;
    target = proto_crc16_ccitt_false(first, 4u);
    for (uint32_t suffix = 0u; suffix <= UINT16_MAX; suffix++) {
        second[2] = (uint8_t)suffix;
        second[3] = (uint8_t)(suffix >> 8);
        if (proto_crc16_ccitt_false(second, 4u) == target) {
            assert(memcmp(first, second, 4u) != 0);
            return;
        }
    }
    assert(false);
}

static void assert_result_id_equal(const struct command_result_id *actual,
                                   const struct command_result_id *expected)
{
    assert(actual->gateway_id == expected->gateway_id);
    assert(actual->gateway_epoch == expected->gateway_epoch);
    assert(actual->command_seq == expected->command_seq);
    assert(actual->node_id == expected->node_id);
    assert(actual->node_boot_counter == expected->node_boot_counter);
    assert(actual->result_seq == expected->result_seq);
}

static void make_two_record_result_bundle(
    uint8_t *payload,
    size_t payload_cap,
    size_t *payload_len,
    struct command_result_id ids[2])
{
    const uint8_t record_payloads[2][3] = {
        {0x11u, 0x12u, 0x13u},
        {0x21u, 0x22u, 0x23u},
    };
    struct result_bundle_header header = {
        .gateway_id = 0x9999888877776666ull,
        .gateway_epoch = 7u,
        .command_seq = 0x12345678u,
        .collection_epoch_id = 0x10203040u,
        .bundle_id = 31u,
        .record_count = 2u,
    };
    uint8_t records[128];
    size_t records_len = 0u;

    assert(payload != NULL);
    assert(payload_len != NULL);
    assert(ids != NULL);
    for (size_t i = 0u; i < 2u; i++) {
        const struct result_bundle_record record = {
            .result_id = {
                .gateway_id = header.gateway_id,
                .gateway_epoch = header.gateway_epoch,
                .command_seq = header.command_seq,
                .node_id = 0x1011121314151600ull + i,
                .node_boot_counter = 41u + (uint32_t)i,
                .result_seq = 51u + (uint16_t)i,
            },
            .payload_len = sizeof(record_payloads[i]),
            .payload_crc = proto_crc16_ccitt_false(
                record_payloads[i], sizeof(record_payloads[i])),
            .payload = record_payloads[i],
        };

        ids[i] = record.result_id;
        assert(result_bundle_record_append_tlv(records,
                                               sizeof(records),
                                               &records_len,
                                               &record) == PROTO_OK);
    }

    *payload_len = 0u;
    header.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(payload,
                                            payload_cap,
                                            payload_len,
                                            &header) == PROTO_OK);
    assert(*payload_len <= payload_cap);
    assert(records_len <= payload_cap - *payload_len);
    memcpy(&payload[*payload_len], records, records_len);
    *payload_len += records_len;
}

static size_t first_result_record_offset(const uint8_t *payload,
                                         size_t payload_len)
{
    size_t offset = 0u;

    while (offset < payload_len) {
        assert(payload_len - offset >= 2u);
        assert(payload_len - offset - 2u >= payload[offset + 1u]);
        if (payload[offset] == TLV_RESULT_RECORD) {
            return offset;
        }
        offset += 2u + payload[offset + 1u];
    }
    assert(false);
    return 0u;
}

static void assert_single_record_bundle_stream(
    const uint8_t *stream_record,
    size_t stream_record_len,
    const struct command_result_id *expected_id)
{
    struct result_bundle_header header;
    struct result_bundle_record record;
    const uint8_t *projected_payload;
    size_t projected_payload_len;
    size_t cursor;

    assert(stream_record != NULL);
    assert(expected_id != NULL);
    assert(stream_record_len >= GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    projected_payload_len = proto_get_u16_le(&stream_record[36]);
    projected_payload = &stream_record[GATEWAY_BLE_STREAM_RECORD_HEADER_LEN];
    assert(stream_record_len ==
           GATEWAY_BLE_STREAM_RECORD_HEADER_LEN + projected_payload_len);
    assert(proto_get_u16_le(&stream_record[38]) ==
           proto_crc16_ccitt_false(projected_payload,
                                   projected_payload_len));
    assert(result_bundle_header_from_tlvs(projected_payload,
                                          projected_payload_len,
                                          &header) == PROTO_OK);
    assert(header.record_count == 1u);
    cursor = first_result_record_offset(projected_payload,
                                        projected_payload_len);
    assert(header.bundle_crc ==
           proto_crc16_ccitt_false(&projected_payload[cursor],
                                   projected_payload_len - cursor));
    assert(result_bundle_record_next_from_tlvs(projected_payload,
                                               projected_payload_len,
                                               &cursor,
                                               &record) == PROTO_OK);
    assert_result_id_equal(&record.result_id, expected_id);
    assert(cursor == projected_payload_len);
}

static void test_allowlist_excludes_mesh_control(void)
{
    assert(gateway_ble_should_stream_packet(MSG_CLICK_REPORT, 0u,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_UWB_REPORT, FLAG_COUNT_AS_CLICK,
                                            GATEWAY_BLE_STREAM_CLASS_UNKNOWN));
    assert(gateway_ble_should_stream_packet(MSG_COMMAND_RESULT, 0u,
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

static void test_retained_result_survives_disconnect_and_priority_pressure(void)
{
    struct gateway_ble_stream_state state;
    const uint8_t *record = NULL;
    size_t record_len = 0u;
    uint8_t payload[4] = {1u, 2u, 3u, 4u};
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 1u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 2u);

    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_retained_packet(
               &state, &result, payload, sizeof(payload),
               10u, 20u, false) == 1);
    assert(state.items[0].retain_until_sent);

    for (uint8_t i = 1u; i < GATEWAY_BLE_STREAM_QUEUE_DEPTH; i++) {
        click.seq = (uint16_t)(i + 1u);
        assert(gateway_ble_stream_enqueue_packet(
                   &state, &click, payload, sizeof(payload),
                   10u, 20u + i, true) == 1);
    }
    click.seq = 99u;
    assert(gateway_ble_stream_enqueue_packet(
               &state, &click, payload, sizeof(payload),
               10u, 30u, true) == -ENOSPC);
    assert(state.items[0].packet.msg_type == MSG_COMMAND_RESULT);
    assert(state.items[0].packet.seq == result.seq);
    assert(state.items[0].retain_until_sent);

    assert(gateway_ble_stream_begin_send_view(
               &state, &record, &record_len) == 0);
    assert(record != NULL);
    assert(record_len > 0u);
    gateway_ble_stream_cancel_send(&state);
    assert(gateway_ble_stream_depth(&state) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH);
    assert(state.items[0].retain_until_sent);

    assert(gateway_ble_stream_begin_send_view(
               &state, &record, &record_len) == 0);
    assert(state.items[0].packet.msg_type == MSG_COMMAND_RESULT);
    gateway_ble_stream_mark_sent(&state, 40u);
    assert(gateway_ble_stream_depth(&state) ==
           GATEWAY_BLE_STREAM_QUEUE_DEPTH - 1u);
    assert(state.items[0].packet.msg_type == MSG_CLICK_REPORT);
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
    struct gateway_command_observability_state observability;
    struct gateway_command_event event = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ACCEPTED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    /* The stream receives an explicit custody decision from its gateway
     * wrapper.  It does not link the publisher runtime just to rediscover the
     * durable shape, so ACK is the wire marker and the retained enqueue API is
     * the ownership marker. */
    struct proto_packet publisher = packet(MSG_GATEWAY_COMMAND_EVENT,
                                           FLAG_GATEWAY_ACK_REQUIRED,
                                           10u);
    struct proto_packet diagnostic = packet(MSG_UWB_ANCHOR_DIAG, 0u, 20u);
    struct proto_packet click = packet(MSG_CLICK_REPORT, 0u, 30u);
    struct proto_packet head;
    uint8_t event_payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    uint8_t payload[1] = {0u};
    size_t event_payload_len = 0u;

    gateway_ble_stream_init(&state);
    gateway_command_observability_init(&observability);
    assert(gateway_command_observability_prepare(
               &observability, &event, false) == 0);
    assert(gateway_command_event_encode(&event,
                                        event_payload,
                                        sizeof(event_payload),
                                        &event_payload_len) == 0);
    publisher.src_id = UINT64_C(0x1111222233334444);
    publisher.dst_id = publisher.src_id;
    publisher.session_id = event.event_seq;
    publisher.seq = (uint16_t)event.event_seq;
    publisher.payload_len = (uint16_t)event_payload_len;
    assert(gateway_ble_stream_enqueue_retained_packet(&state, &publisher,
                                                      event_payload,
                                                      event_payload_len,
                                                      0u, 1u, true) == 1);
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

static void test_generic_command_event_does_not_claim_host_custody(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_command_observability_state observability;
    struct gateway_command_event event = {
        .kind = GATEWAY_COMMAND_EVENT_KIND_ANCHOR_ENUMERATION,
        .stage = GATEWAY_COMMAND_EVENT_STAGE_ACCEPTED,
        .status = COMMAND_OK,
        .reason = GATEWAY_COMMAND_EVENT_REASON_NONE,
        .command_id = CMD_ASSIGN_DISCOVERY_SLOTS,
        .slot = GATEWAY_COMMAND_EVENT_SLOT_UNAVAILABLE,
    };
    struct proto_packet generic = packet(MSG_GATEWAY_COMMAND_EVENT, 0u, 11u);
    uint8_t event_payload[GATEWAY_COMMAND_EVENT_WIRE_LEN];
    size_t event_payload_len = 0u;

    gateway_ble_stream_init(&state);
    gateway_command_observability_init(&observability);
    assert(gateway_command_observability_prepare(
               &observability, &event, false) == 0);
    assert(gateway_command_event_encode(&event,
                                        event_payload,
                                        sizeof(event_payload),
                                        &event_payload_len) == 0);
    generic.src_id = UINT64_C(0x1111222233334444);
    generic.dst_id = generic.src_id;
    generic.session_id = event.event_seq;
    generic.seq = (uint16_t)event.event_seq;
    generic.payload_len = (uint16_t)event_payload_len;

    assert(gateway_ble_stream_enqueue_packet(&state, &generic,
                                             event_payload,
                                             event_payload_len,
                                             0u, 1u, true) == 1);
    assert(state.count == 1u);
    assert(!state.items[0].retain_until_sent);
    assert(!state.items[0].host_custody_owner);
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
    command_event.src_id = UINT64_C(0x1111222233334444);
    command_event.dst_id = command_event.src_id;
    command_event.session_id = event.event_seq;
    command_event.seq = (uint16_t)event.event_seq;
    command_event.payload_len = (uint16_t)event_payload_len;
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

static void test_corrupt_item_extent_cannot_underflow_pool_compaction(void)
{
    struct gateway_ble_stream_state state;
    struct gateway_ble_stream_diagnostics diag;
    uint8_t payload[3] = {1u, 2u, 3u};
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 1u);
    uint16_t pool_used;

    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             110u,
                                             true) == 1);
    pool_used = state.pool_used;
    state.items[0].offset = (uint16_t)(pool_used + 1u);
    gateway_ble_stream_mark_sent(&state, 120u);
    gateway_ble_stream_get_diagnostics(&state, 120u, &diag);
    assert(state.count == 1u);
    assert(state.pool_used == pool_used);
    assert(diag.packets_sent == 0u);
    assert(diag.bytes_sent == 0u);

    state.items[0].offset = 0u;
    state.items[0].len = (uint16_t)(pool_used + 1u);
    gateway_ble_stream_mark_sent(&state, 130u);
    gateway_ble_stream_get_diagnostics(&state, 130u, &diag);
    assert(state.count == 1u);
    assert(state.pool_used == pool_used);
    assert(diag.packets_sent == 0u);
    assert(diag.bytes_sent == 0u);
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

static void test_result_bundle_projection_preserves_raw_reservation_identity(void)
{
    struct gateway_ble_stream_state state;
    struct command_result_id ids[2] = {0};
    struct proto_packet bundle = packet(
        MSG_RESULT_BUNDLE, FLAG_GATEWAY_ACK_REQUIRED, 31u);
    uint8_t raw_payload[256];
    const uint8_t *stream_record = NULL;
    size_t raw_payload_len = 0u;
    size_t stream_record_len = 0u;

    make_two_record_result_bundle(raw_payload,
                                  sizeof(raw_payload),
                                  &raw_payload_len,
                                  ids);
    bundle.payload_len = (uint16_t)raw_payload_len;
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &bundle,
                                             raw_payload,
                                             raw_payload_len,
                                             100u,
                                             110u,
                                             true) == 1);

    /* An out-of-range selection fails closed without consuming custody. */
    assert(gateway_ble_stream_commit_bundle_projection_reservation(
               &state,
               &bundle,
               raw_payload,
               raw_payload_len,
               0x04u) == -EBADMSG);
    assert(state.reservation_active);
    assert(state.count == 0u);

    assert(gateway_ble_stream_commit_bundle_projection_reservation(
               &state,
               &bundle,
               raw_payload,
               raw_payload_len,
               0x02u) == 1);
    assert(!state.reservation_active);
    assert(state.count == 1u);
    assert(state.items[0].packet.msg_type == MSG_RESULT_BUNDLE);
    assert(state.items[0].packet.payload_len == raw_payload_len);
    assert(gateway_ble_stream_peek(&state,
                                   &stream_record,
                                   &stream_record_len) == 0);
    assert_single_record_bundle_stream(stream_record,
                                       stream_record_len,
                                       &ids[1]);
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

static void test_host_receipt_phase_retains_exact_stream_head(void)
{
    struct gateway_ble_stream_state state;
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 45u);
    struct proto_packet other = packet(MSG_ANCHOR_HEARTBEAT, 0u, 46u);
    struct proto_packet head_packet;
    uint8_t payload[7] = {1u, 3u, 5u, 7u, 9u, 11u, 13u};
    const uint8_t *record = NULL;
    size_t record_len = 0u;

    result.payload_len = sizeof(payload);
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_mark_host_notified(&state) == -ENOENT);
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &result,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             123u,
                                             true) == 1);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 payload,
                                                 sizeof(payload)) == 1);
    assert(gateway_ble_stream_mark_host_notified(&state) == -EAGAIN);
    assert(gateway_ble_stream_head_packet(&state, &head_packet) == 0);
    assert(head_packet.seq == result.seq);

    assert(gateway_ble_stream_begin_send_view(&state,
                                              &record,
                                              &record_len) == 0);
    assert(record != NULL);
    assert(record_len > GATEWAY_BLE_STREAM_RECORD_HEADER_LEN);
    assert(gateway_ble_stream_mark_host_notified(&state) == 0);

    /* A disconnect/timeout rewinds only the unreceipted host boundary. */
    gateway_ble_stream_cancel_send(&state);
    assert(state.head_send_phase ==
           GATEWAY_BLE_STREAM_HEAD_HOST_NOTIFIED);
    assert(gateway_ble_stream_rewind_host_notification(&state) == 0);
    assert(state.head_send_phase == GATEWAY_BLE_STREAM_HEAD_IDLE);
    assert(gateway_ble_stream_begin_send_view(&state,
                                              &record,
                                              &record_len) == 0);
    assert(gateway_ble_stream_mark_host_notified(&state) == 0);
    assert(gateway_ble_stream_begin_send_view(&state,
                                              &record,
                                              &record_len) == -EBUSY);

    /* Priority pressure cannot evict the exact retained stream head either. */
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             124u,
                                             true) == 1);
    assert(gateway_ble_stream_depth(&state) == 2u);
    {
        struct proto_packet retained_packet;
        assert(gateway_ble_stream_head_packet(&state, &retained_packet) == 0);
        assert(retained_packet.seq == head_packet.seq);
    }
    assert(gateway_ble_stream_accept_host_receipt(&state) == 0);
    assert(state.head_send_phase ==
           GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED);
    assert(gateway_ble_stream_rewind_host_notification(&state) == -EALREADY);
    assert(state.head_send_phase ==
           GATEWAY_BLE_STREAM_HEAD_HOST_ACCEPTED);
    gateway_ble_stream_mark_sent(&state, 125u);
    assert(gateway_ble_stream_depth(&state) == 1u);
    assert(state.items[0].packet.seq == other.seq);

    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_enqueue_packet(&state,
                                             &other,
                                             payload,
                                             sizeof(payload),
                                             100u,
                                             125u,
                                             true) == 1);
    assert(gateway_ble_stream_begin_send_view(&state,
                                              &record,
                                              &record_len) == 0);
    assert(gateway_ble_stream_mark_host_notified(&state) == -EPERM);
    gateway_ble_stream_cancel_send(&state);
    gateway_ble_stream_mark_sent(&state, 126u);
}

static void test_reservation_rejects_crc16_collision(void)
{
    struct gateway_ble_stream_state state;
    struct proto_packet result = packet(MSG_COMMAND_RESULT, 0u, 42u);
    uint8_t first[4];
    uint8_t collision[4];

    make_crc16_collision(first, collision);
    assert(proto_crc16_ccitt_false(first, sizeof(first)) ==
           proto_crc16_ccitt_false(collision, sizeof(collision)));
    result.payload_len = sizeof(first);
    gateway_ble_stream_init(&state);
    assert(gateway_ble_stream_reserve_packet(&state,
                                             &result,
                                             first,
                                             sizeof(first),
                                             100u,
                                             123u,
                                             true) == 1);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 collision,
                                                 sizeof(collision)) ==
           -ESTALE);
    assert(state.reservation_active);
    assert(gateway_ble_stream_commit_reservation(&state,
                                                 &result,
                                                 first,
                                                 sizeof(first)) == 1);
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

static void test_direct_queue_restarts_exact_head_after_disconnect(void)
{
    struct gateway_ble_direct_queue_state state;
    const uint8_t frames[2][3] = {
        {'A', '0', '\0'},
        {'B', '0', '\0'},
    };
    uint8_t stored[2][3] = {{0}};
    uint8_t slot = UINT8_MAX;

    gateway_ble_direct_queue_init(&state);
    assert(gateway_ble_direct_queue_enqueue(&state, 2u, &slot) == 0);
    assert(slot == 0u);
    memcpy(stored[slot], frames[0], sizeof(stored[slot]));
    assert(gateway_ble_direct_queue_enqueue(&state, 2u, &slot) == 0);
    assert(slot == 1u);
    memcpy(stored[slot], frames[1], sizeof(stored[slot]));

    assert(gateway_ble_direct_queue_begin(&state, 2u, &slot) == 0);
    assert(slot == 0u);
    assert(memcmp(stored[slot], frames[0], sizeof(frames[0])) == 0);
    /* A notification prefix may have completed, but a disconnect cancels
     * only transfer progress.  It must not remove A or expose B. */
    gateway_ble_direct_queue_cancel(&state);
    assert(state.count == 2u);
    assert(gateway_ble_direct_queue_begin(&state, 2u, &slot) == 0);
    assert(slot == 0u);
    assert(memcmp(stored[slot], frames[0], sizeof(frames[0])) == 0);
    assert(gateway_ble_direct_queue_complete(&state, 2u) == 0);

    assert(gateway_ble_direct_queue_begin(&state, 2u, &slot) == 0);
    assert(slot == 1u);
    assert(memcmp(stored[slot], frames[1], sizeof(frames[1])) == 0);
    assert(gateway_ble_direct_queue_complete(&state, 2u) == 0);
    assert(state.count == 0u);
}

static void test_direct_queue_refusal_preserves_active_head_and_order(void)
{
    struct gateway_ble_direct_queue_state state;
    uint8_t slot = UINT8_MAX;

    gateway_ble_direct_queue_init(&state);
    assert(gateway_ble_direct_queue_enqueue(&state, 2u, &slot) == 0);
    assert(slot == 0u);
    assert(gateway_ble_direct_queue_enqueue(&state, 2u, &slot) == 0);
    assert(slot == 1u);
    assert(gateway_ble_direct_queue_begin(&state, 2u, &slot) == 0);
    assert(slot == 0u);

    slot = UINT8_MAX;
    assert(gateway_ble_direct_queue_enqueue(&state, 2u, &slot) == -ENOSPC);
    assert(slot == UINT8_MAX);
    assert(state.head == 0u);
    assert(state.count == 2u);
    assert(state.head_active);
    assert(gateway_ble_direct_queue_complete(&state, 2u) == 0);
    assert(gateway_ble_direct_queue_begin(&state, 2u, &slot) == 0);
    assert(slot == 1u);
}

struct retained_bulk_capture {
    uint16_t accepted[8u * 100u];
    size_t accepted_count;
    uint8_t refusal_phase;
    uint32_t waits;
};

static int retained_bulk_send(const uint8_t *frame,
                              size_t frame_len,
                              void *ctx)
{
    struct retained_bulk_capture *capture = ctx;
    uint16_t identity;

    assert(frame != NULL);
    assert(frame_len == sizeof(identity));
    identity = proto_get_u16_le(frame);
    assert(identity == capture->accepted_count);
    if (capture->refusal_phase == 0u) {
        return -ENOTCONN;
    }
    if (capture->refusal_phase == 1u) {
        return -ENOSPC;
    }

    capture->accepted[capture->accepted_count++] = identity;
    capture->refusal_phase = 0u;
    return 0;
}

static void retained_bulk_wait(void *ctx)
{
    struct retained_bulk_capture *capture = ctx;

    capture->waits++;
    capture->refusal_phase++;
}

static void test_retained_bulk_preserves_full_eight_by_hundred_order(void)
{
    struct retained_bulk_capture capture = {0};

    for (uint16_t identity = 0u; identity < 8u * 100u; identity++) {
        uint8_t frame[sizeof(identity)];
        uint32_t retries = 0u;

        proto_put_u16_le(frame, identity);
        assert(gateway_ble_send_frame_retained(frame,
                                               sizeof(frame),
                                               retained_bulk_send,
                                               retained_bulk_wait,
                                               &capture,
                                               &retries) == 0);
        assert(retries == 2u);
    }
    assert(capture.accepted_count == 8u * 100u);
    assert(capture.waits == 2u * 8u * 100u);
    for (uint16_t identity = 0u; identity < 8u * 100u; identity++) {
        assert(capture.accepted[identity] == identity);
    }
}

static void test_work_handoff_failure_requires_watchdog_owner(void)
{
    assert(!gateway_ble_work_handoff_requires_reset(0));
    assert(!gateway_ble_work_handoff_requires_reset(1));
    assert(!gateway_ble_work_handoff_requires_reset(2));
    assert(gateway_ble_work_handoff_requires_reset(-EBUSY));
    assert(gateway_ble_work_handoff_requires_reset(-ENODEV));
}

int main(void)
{
    test_allowlist_excludes_mesh_control();
    test_queue_full_counts_drop();
    test_max_size_click_is_preserved_and_oversize_is_rejected();
    test_disconnected_full_queue_counts_not_ready();
    test_retained_result_survives_disconnect_and_priority_pressure();
    test_click_evicts_lower_priority_diagnostic();
    test_click_cannot_evict_durable_result_records();
    test_retained_assignment_event_survives_click_eviction();
    test_generic_command_event_does_not_claim_host_custody();
    test_terminal_command_event_evicts_lower_priority_status();
    test_fast_drain_and_counters();
    test_corrupt_item_extent_cannot_underflow_pool_compaction();
    test_active_head_cannot_be_evicted();
    test_pool_holds_core_click_and_two_cir_records();
    test_pool_boundary_preserves_retained_click_custody();
    test_result_bundle_projection_preserves_raw_reservation_identity();
    test_reservation_reports_full_queue_backpressure();
    test_reservation_protects_capacity_and_cancel_releases_it();
    test_reservation_commit_is_exact_and_single_use();
    test_host_receipt_phase_retains_exact_stream_head();
    test_reservation_rejects_crc16_collision();
    test_reservation_uses_priority_eviction_and_survives_drain();
    test_ble_recovery_backoff_is_random_exponential_and_capped();
    test_direct_queue_restarts_exact_head_after_disconnect();
    test_direct_queue_refusal_preserves_active_head_and_order();
    test_retained_bulk_preserves_full_eight_by_hundred_order();
    test_work_handoff_failure_requires_watchdog_owner();
    return 0;
}
