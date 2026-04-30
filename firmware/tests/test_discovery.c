#include "discovery.h"

#include <assert.h>

static void test_request_round_trip_click(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .event_seq = 42u,
        .flags = ble_flags_for_click(),
    };
    struct ble_discovery_req decoded = {0};
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == BLE_DISCOVERY_REQ_LEN);
    assert(proto_get_u16_le(buf) == BLE_COMPANY_ID);
    assert(buf[2] == PROTO_VERSION);
    assert(buf[3] == MSG_BLE_DISCOVERY_REQ);

    assert(ble_discovery_req_decode(buf, written, &decoded) == PROTO_OK);
    assert(decoded.clicker_id == request.clicker_id);
    assert(decoded.event_seq == request.event_seq);
    assert(decoded.flags == request.flags);
    assert(ble_flags_count_as_click(decoded.flags));
    assert(!ble_flags_are_diagnostic(decoded.flags));
}

static void test_request_rejects_diagnostic_click_mix(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .event_seq = 42u,
        .flags = FLAG_DIAGNOSTIC | FLAG_COUNT_AS_CLICK,
    };
    uint8_t buf[BLE_DISCOVERY_REQ_LEN];
    size_t written = 0u;

    assert(ble_discovery_req_encode(&request, buf, sizeof(buf), &written) == PROTO_ERR_MALFORMED);
}

static void test_ready_round_trip_diagnostic(void)
{
    const struct ble_discovery_ready ready = {
        .anchor_id = 0x8877665544332211ull,
        .uwb_short_addr = 0xBEEF,
        .flags = ble_flags_for_diagnostic(),
        .rssi_hint = -63,
    };
    struct ble_discovery_ready decoded = {0};
    uint8_t buf[BLE_DISCOVERY_READY_LEN];
    size_t written = 0u;

    assert(ble_discovery_ready_encode(&ready, buf, sizeof(buf), &written) == PROTO_OK);
    assert(written == BLE_DISCOVERY_READY_LEN);
    assert(buf[3] == MSG_BLE_DISCOVERY_READY);

    assert(ble_discovery_ready_decode(buf, written, &decoded) == PROTO_OK);
    assert(decoded.anchor_id == ready.anchor_id);
    assert(decoded.uwb_short_addr == ready.uwb_short_addr);
    assert(decoded.flags == ready.flags);
    assert(decoded.rssi_hint == ready.rssi_hint);
    assert(ble_flags_are_diagnostic(decoded.flags));
    assert(!ble_flags_count_as_click(decoded.flags));
}

static void test_decode_rejects_wrong_version(void)
{
    const struct ble_discovery_req request = {
        .clicker_id = 0x1122334455667788ull,
        .event_seq = 42u,
        .flags = ble_flags_for_click(),
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
    test_ready_round_trip_diagnostic();
    test_decode_rejects_wrong_version();
    return 0;
}
