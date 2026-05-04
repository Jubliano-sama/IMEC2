#include "discovery.h"

#include <assert.h>

static void test_request_round_trip_click(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .priority_id = 0x0102030405060708ull,
        .event_seq = 42u,
        .ready_scan_starts_in_ms = 330u,
        .ready_scan_duration_ms = 200u,
        .flags = ble_flags_for_click(),
        .attempt_index = 3u,
        .min_anchor_count = 4u,
    };
    struct ble_discovery_req decoded = {0};
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == BLE_DISCOVERY_REQ_LEN);
    assert(proto_get_u16_le(buf) == BLE_COMPANY_ID);
    assert(buf[2] == PROTO_VERSION);
    assert(buf[3] == MSG_BLE_DISCOVERY_REQ);
    assert(buf[17] == request.attempt_index);
    assert(proto_get_u16_le(&buf[18]) == request.ready_scan_starts_in_ms);
    assert(proto_get_u16_le(&buf[20]) == request.ready_scan_duration_ms);
    assert(buf[22] == request.min_anchor_count);
    assert(proto_get_u64_le(&buf[23]) == request.priority_id);

    assert(ble_discovery_req_decode(buf, written, &decoded) == PROTO_OK);
    assert(decoded.clicker_id == request.clicker_id);
    assert(decoded.priority_id == request.priority_id);
    assert(decoded.event_seq == request.event_seq);
    assert(decoded.ready_scan_starts_in_ms == request.ready_scan_starts_in_ms);
    assert(decoded.ready_scan_duration_ms == request.ready_scan_duration_ms);
    assert(decoded.flags == request.flags);
    assert(decoded.attempt_index == request.attempt_index);
    assert(decoded.min_anchor_count == request.min_anchor_count);
    assert(ble_flags_count_as_click(decoded.flags));
    assert(!ble_flags_are_diagnostic(decoded.flags));
}

static void test_request_rejects_diagnostic_click_mix(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .priority_id = 0x0102030405060708ull,
        .event_seq = 42u,
        .ready_scan_starts_in_ms = 330u,
        .ready_scan_duration_ms = 200u,
        .flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK,
        .attempt_index = 1u,
        .min_anchor_count = 4u,
    };
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
}

static void test_request_rejects_missing_ready_timing(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .priority_id = 0x0102030405060708ull,
        .event_seq = 42u,
        .ready_scan_starts_in_ms = 0u,
        .ready_scan_duration_ms = 200u,
        .flags = ble_flags_for_click(),
        .attempt_index = 1u,
        .min_anchor_count = 4u,
    };
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
}

static void test_ready_round_trip_diagnostic(void)
{
    const struct ble_discovery_ready ready = {
        .anchor_id = 0x8877665544332211ull,
        .target_clicker_id = 0x1122334455667788ull,
        .priority_id_seen = 0x0102030405060708ull,
        .target_event_seq = 42u,
        .uwb_short_addr = 0xBEEF,
        .flags = ble_flags_for_diagnostic(),
        .attempt_index = 3u,
        .rssi_hint = -63,
        .status = BLE_READY_STATUS_ADMITTED,
    };
    struct ble_discovery_ready decoded = {0};
    uint8_t buf[BLE_DISCOVERY_READY_LEN];
    size_t written = 0u;

    assert(ble_discovery_ready_encode(&ready, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == BLE_DISCOVERY_READY_LEN);
    assert(buf[3] == MSG_BLE_DISCOVERY_READY);

    assert(ble_discovery_ready_decode(buf, written, &decoded) == PROTO_OK);
    assert(decoded.anchor_id == ready.anchor_id);
    assert(decoded.target_clicker_id == ready.target_clicker_id);
    assert(decoded.priority_id_seen == ready.priority_id_seen);
    assert(decoded.target_event_seq == ready.target_event_seq);
    assert(decoded.uwb_short_addr == ready.uwb_short_addr);
    assert(decoded.flags == ready.flags);
    assert(decoded.attempt_index == ready.attempt_index);
    assert(decoded.rssi_hint == ready.rssi_hint);
    assert(decoded.status == ready.status);
    assert(ble_flags_are_diagnostic(decoded.flags));
    assert(!ble_flags_count_as_click(decoded.flags));
}

static void test_decode_rejects_wrong_version(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .priority_id = 0x0102030405060708ull,
        .event_seq = 42u,
        .ready_scan_starts_in_ms = 330u,
        .ready_scan_duration_ms = 200u,
        .flags = ble_flags_for_click(),
        .attempt_index = 1u,
        .min_anchor_count = 4u,
    };
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_OK);
    buf[2] = 0xFFu;
    assert(ble_discovery_req_decode(buf, written, &(struct ble_discovery_req){0}) == PROTO_ERR_BAD_VERSION);
}

int main(void)
{
    test_request_round_trip_click();
    test_request_rejects_diagnostic_click_mix();
    test_request_rejects_missing_ready_timing();
    test_ready_round_trip_diagnostic();
    test_decode_rejects_wrong_version();
    return 0;
}
