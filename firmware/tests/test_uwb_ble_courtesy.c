#include "uwb_ble_courtesy.h"

#include <assert.h>
#include <stdint.h>

static struct uwb_ble_courtesy_frame courtesy_frame(void)
{
    const struct uwb_ble_courtesy_frame frame = {
        .network_id = 0x494d4543u,
        .clicker_id = UINT64_C(0x0102030405060708),
        .click_event_id = 42u,
        .attempt_index = 2u,
        .priority_id = UINT64_C(0x1111222233334444),
        .defer_duration_units = 84u,
    };
    return frame;
}

static void test_duration_units(void)
{
    assert(UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN + 2u <= 31u);
    assert(UWB_BLE_COURTESY_DURATION_UNIT_MS == 10u);
    assert(uwb_ble_courtesy_duration_units_from_ms(0u) == 0u);
    assert(uwb_ble_courtesy_duration_units_from_ms(1u) == 1u);
    assert(uwb_ble_courtesy_duration_units_from_ms(10u) == 1u);
    assert(uwb_ble_courtesy_duration_units_from_ms(11u) == 2u);
    assert(uwb_ble_courtesy_duration_units_from_ms(UWB_BLE_COURTESY_MAX_DURATION_MS + 1u) ==
           UINT8_MAX);
    assert(uwb_ble_courtesy_duration_ms(84u) == 840u);
}

static void test_round_trip(void)
{
    const struct uwb_ble_courtesy_frame frame = courtesy_frame();
    struct uwb_ble_courtesy_frame decoded = {0};
    uint8_t raw[UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN] = {0};
    size_t written = 0u;

    assert(uwb_ble_courtesy_encode(&frame, raw, sizeof(raw), &written) == PROTO_OK);
    assert(written == UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN);
    assert(proto_get_u16_le(&raw[0]) == UWB_BLE_COURTESY_COMPANY_ID);
    assert(raw[2] == UWB_BLE_COURTESY_MARKER_VERSION);

    assert(uwb_ble_courtesy_decode(raw, written, &decoded) == PROTO_OK);
    assert(decoded.network_id == frame.network_id);
    assert(decoded.clicker_id == frame.clicker_id);
    assert(decoded.click_event_id == frame.click_event_id);
    assert(decoded.attempt_index == frame.attempt_index);
    assert(decoded.priority_id == frame.priority_id);
    assert(decoded.defer_duration_units == frame.defer_duration_units);
    assert(raw[28] == frame.defer_duration_units);
}

static void test_rejects_malformed_payloads(void)
{
    struct uwb_ble_courtesy_frame frame = courtesy_frame();
    uint8_t raw[UWB_BLE_COURTESY_MANUFACTURER_DATA_LEN] = {0};
    size_t written = 0u;

    assert(uwb_ble_courtesy_encode(NULL, raw, sizeof(raw), &written) == PROTO_ERR_ARG);
    assert(uwb_ble_courtesy_encode(&frame, NULL, sizeof(raw), &written) == PROTO_ERR_ARG);
    assert(uwb_ble_courtesy_encode(&frame, raw, 1u, &written) == PROTO_ERR_NO_SPACE);

    frame.attempt_index = 0u;
    assert(uwb_ble_courtesy_encode(&frame, raw, sizeof(raw), &written) ==
           PROTO_ERR_MALFORMED);

    frame = courtesy_frame();
    frame.defer_duration_units = 0u;
    assert(uwb_ble_courtesy_encode(&frame, raw, sizeof(raw), &written) ==
           PROTO_ERR_MALFORMED);

    frame = courtesy_frame();
    assert(uwb_ble_courtesy_encode(&frame, raw, sizeof(raw), &written) == PROTO_OK);
    assert(uwb_ble_courtesy_decode(raw, written - 1u, &frame) == PROTO_ERR_BAD_LENGTH);

    raw[2] ^= 0xffu;
    assert(uwb_ble_courtesy_decode(raw, written, &frame) == PROTO_ERR_BAD_MAGIC);
}

static void test_attempt_first_precedence(void)
{
    assert(uwb_claim_precedence_compare(2u,
                                        UINT64_C(0xffffffffffffffff),
                                        20u,
                                        20u,
                                        1u,
                                        0u,
                                        1u,
                                        1u) > 0);
    assert(uwb_claim_precedence_compare(1u,
                                        0u,
                                        1u,
                                        1u,
                                        2u,
                                        UINT64_C(0xffffffffffffffff),
                                        20u,
                                        20u) < 0);
}

static void test_same_attempt_tie_breakers(void)
{
    assert(uwb_claim_precedence_compare(1u, 4u, 10u, 10u,
                                        1u, 5u, 1u, 1u) > 0);
    assert(uwb_claim_precedence_compare(1u, 4u, 10u, 10u,
                                        1u, 4u, 9u, 10u) < 0);
    assert(uwb_claim_precedence_compare(1u, 4u, 10u, 10u,
                                        1u, 4u, 10u, 9u) < 0);
    assert(uwb_claim_precedence_compare(1u, 4u, 10u, 10u,
                                        1u, 4u, 10u, 10u) == 0);
}

int main(void)
{
    test_duration_units();
    test_round_trip();
    test_rejects_malformed_payloads();
    test_attempt_first_precedence();
    test_same_attempt_tie_breakers();
    return 0;
}
