#include "survey.h"

#include <assert.h>

static struct survey_sample sample(void)
{
    struct survey_sample value = {
        .pair = {
            .survey_id = 0xAABBCCDDu,
            .initiator_id = 0x1111222233334444ull,
            .responder_id = 0x5555666677778888ull,
            .sample_count = 3u,
        },
        .sample_index = 1u,
        .distance_mm = -1234,
        .quality = 88u,
        .range_status = RANGE_OK,
    };
    return value;
}

static void test_sample_count_validation(void)
{
    assert(!survey_sample_count_valid(0u));
    assert(survey_sample_count_valid(1u));
    assert(survey_sample_count_valid(1000u));
    assert(!survey_sample_count_valid(1001u));
}

static void test_pair_and_sample_validation(void)
{
    struct survey_sample value = sample();

    assert(survey_sample_validate(&value) == PROTO_OK);

    value.sample_index = value.pair.sample_count;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.quality = 101u;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);

    value = sample();
    value.pair.responder_id = value.pair.initiator_id;
    assert(survey_sample_validate(&value) == PROTO_ERR_MALFORMED);
}

static void expect_u64_tlv(const uint8_t *payload, size_t payload_len, uint8_t type, uint64_t expected)
{
    const uint8_t *value = NULL;
    uint8_t len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &len) == PROTO_OK);
    assert(len == 8u);
    assert(proto_get_u64_le(value) == expected);
}

static void test_sample_tlvs_include_required_fields(void)
{
    uint8_t payload[128];
    size_t payload_len = 0u;
    const struct survey_sample value = sample();
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    assert(survey_append_sample_tlvs(payload, sizeof(payload), &payload_len, &value) == PROTO_OK);

    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == value.pair.survey_id);

    expect_u64_tlv(payload, payload_len, TLV_INITIATOR_ID, value.pair.initiator_id);
    expect_u64_tlv(payload, payload_len, TLV_RESPONDER_ID, value.pair.responder_id);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_INDEX, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == value.sample_index);

    assert(tlv_find(payload, payload_len, TLV_SAMPLE_COUNT, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 2u);
    assert(proto_get_u16_le(tlv_value) == value.pair.sample_count);

    assert(tlv_find(payload, payload_len, TLV_DISTANCE_MM, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == (uint32_t)value.distance_mm);

    assert(tlv_find(payload, payload_len, TLV_QUALITY, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 1u);
    assert(tlv_value[0] == value.quality);

    assert(tlv_find(payload, payload_len, TLV_RANGE_STATUS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 1u);
    assert(tlv_value[0] == (uint8_t)value.range_status);
}

static void test_reach_request_tlvs_include_survey_and_duration(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;

    assert(survey_append_reach_request_tlvs(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            0xABCDEF01u,
                                            250u) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 0xABCDEF01u);
    assert(tlv_find(payload, payload_len, TLV_DURATION_MS, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 250u);
}

static void test_reach_report_tlvs_include_peer_entries(void)
{
    const struct survey_reachability_entry entries[] = {
        {
            .peer_id = 0x1111222233334444ull,
            .rssi_dbm = -61,
            .quality = 82u,
        },
        {
            .peer_id = 0x5555666677778888ull,
            .rssi_dbm = -74,
            .quality = 63u,
        },
    };
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *tlv_value = NULL;
    uint8_t tlv_len = 0u;
    uint8_t entry_count = 0u;

    assert(survey_append_reach_report_tlvs(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           0xAABBCCDDu,
                                           0x9999888877776666ull,
                                           entries,
                                           sizeof(entries) / sizeof(entries[0])) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_SURVEY_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 4u);
    assert(proto_get_u32_le(tlv_value) == 0xAABBCCDDu);
    assert(tlv_find(payload, payload_len, TLV_ANCHOR_ID, &tlv_value, &tlv_len) == PROTO_OK);
    assert(tlv_len == 8u);
    assert(proto_get_u64_le(tlv_value) == 0x9999888877776666ull);

    for (size_t offset = 0u; offset < payload_len;) {
        uint8_t type = payload[offset];
        uint8_t len = payload[offset + 1u];

        offset += 2u;
        if (type == TLV_REACHABILITY_ENTRY) {
            assert(len == SURVEY_REACHABILITY_ENTRY_LEN);
            assert(proto_get_u64_le(&payload[offset]) == entries[entry_count].peer_id);
            assert((int8_t)payload[offset + 8u] == entries[entry_count].rssi_dbm);
            assert(payload[offset + 9u] == entries[entry_count].quality);
            entry_count++;
        }
        offset += len;
    }
    assert(entry_count == 2u);
}

static void test_reach_report_packet_is_diagnostic_gateway_bound(void)
{
    struct proto_packet packet = {0};

    assert(survey_init_reach_report_packet(&packet,
                                           0x1111222233334444ull,
                                           0x9999888877776666ull,
                                           0xAABBCCDDu,
                                           11u,
                                           32u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_REACH_REPORT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == 0x1111222233334444ull);
    assert(packet.dst_id == 0x9999888877776666ull);
    assert(packet.session_id == 0xAABBCCDDu);
    assert(packet.seq == 11u);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 32u);
}

static void test_result_packet_is_diagnostic_not_click(void)
{
    struct proto_packet packet = {0};
    const struct survey_sample value = sample();

    assert(survey_init_result_packet(&packet, &value, 0x9999888877776666ull, 42u, 77u) == PROTO_OK);
    assert(packet.msg_type == MSG_SURVEY_PAIR_RESULT);
    assert((packet.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((packet.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((packet.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((packet.flags & FLAG_COUNT_AS_CLICK) == 0u);
    assert(packet.src_id == value.pair.initiator_id);
    assert(packet.session_id == value.pair.survey_id);
    assert(packet.ttl == SURVEY_DEFAULT_TTL);
    assert(packet.payload_len == 77u);
}

int main(void)
{
    test_sample_count_validation();
    test_pair_and_sample_validation();
    test_sample_tlvs_include_required_fields();
    test_reach_request_tlvs_include_survey_and_duration();
    test_reach_report_tlvs_include_peer_entries();
    test_reach_report_packet_is_diagnostic_gateway_bound();
    test_result_packet_is_diagnostic_not_click();
    return 0;
}
