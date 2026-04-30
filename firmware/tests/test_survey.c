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
    test_result_packet_is_diagnostic_not_click();
    return 0;
}
