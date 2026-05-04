#include "report.h"

#include <assert.h>

static void test_click_report_packet_counts_as_click(void)
{
    const struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 95u,
        .rsl_dbm = -73,
        .range_status = RANGE_OK,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(report_init_click_packet(&packet,
                                         fields.anchor_id,
                                         0x9999888877776666ull,
                                         0x12345678u,
                                         1u,
                                         (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_CLICK_REPORT);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) != 0u);
    assert((packet.flags & FLAG_DIAGNOSTIC) == 0u);
    assert((packet.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);

    assert(tlv_find(payload, payload_len, TLV_CLICKER_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == fields.clicker_id);

    assert(tlv_find(payload, payload_len, TLV_TIMESTAMP_MS, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == fields.timestamp_ms);

    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == RANGE_OK);

    assert(tlv_find(payload, payload_len, TLV_UWB_RSL_DBM, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert((int8_t)value[0] == fields.rsl_dbm);
}

static void test_self_test_report_is_diagnostic_not_click(void)
{
    const struct self_test_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .event_seq = 124u,
        .failure_code = 5u,
        .battery_mv = 3710u,
    };
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(report_append_self_test_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_OK);
    assert(report_init_self_test_packet(&packet,
                                             fields.clicker_id,
                                             0x9999888877776666ull,
                                             0x12345678u,
                                             2u,
                                             (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_SELF_TEST_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);

    assert(tlv_find(payload, payload_len, TLV_ERROR_CODE, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == fields.failure_code);
}

static void test_rejects_bad_range_fields(void)
{
    struct range_report_fields fields = {
        .clicker_id = 0x1111222233334444ull,
        .anchor_id = 0x5555666677778888ull,
        .event_seq = 123u,
        .timestamp_ms = 987654u,
        .distance_mm = 4567,
        .quality = 101u,
        .range_status = RANGE_OK,
    };
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(report_append_range_tlvs(payload, sizeof(payload), &payload_len, &fields) == PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_click_report_packet_counts_as_click();
    test_self_test_report_is_diagnostic_not_click();
    test_rejects_bad_range_fields();
    return 0;
}
