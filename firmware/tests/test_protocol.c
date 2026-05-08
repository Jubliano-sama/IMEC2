#include "protocol.h"

#include <assert.h>
#include <string.h>

static void test_crc_known_vector(void)
{
    const uint8_t data[] = "123456789";
    assert(proto_crc16_ccitt_false(data, 9u) == 0x29B1u);
}

static void test_tlv_and_packet_round_trip(void)
{
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint8_t packet_buf[PACKET_MAX_LEN];
    size_t packet_len = 0u;
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_append_u32(payload, sizeof(payload), &payload_len, TLV_EVENT_SEQ, 42u) == PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len, TLV_CLICKER_ID, 0x1122334455667788ull) == PROTO_OK);
    assert(tlv_append_i32(payload, sizeof(payload), &payload_len, TLV_DISTANCE_MM, -1234) == PROTO_OK);

    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = 0x1122334455667788ull,
        .dst_id = 0,
        .session_id = 0xAABBCCDDu,
        .seq = 17u,
        .ttl = 4u,
        .payload_len = (uint8_t)payload_len,
    };

    assert(proto_packet_encode(&packet, payload, packet_buf, sizeof(packet_buf), &packet_len) == PROTO_OK);
    assert(packet_len == PACKET_HEADER_LEN + payload_len + PACKET_CRC_LEN);

    struct proto_packet decoded = {0};
    assert(proto_packet_decode(packet_buf, packet_len, &decoded, &decoded_payload, &decoded_payload_len) == PROTO_OK);
    assert(decoded.msg_type == packet.msg_type);
    assert(decoded.flags == packet.flags);
    assert(decoded.src_id == packet.src_id);
    assert(decoded.dst_id == packet.dst_id);
    assert(decoded.session_id == packet.session_id);
    assert(decoded.seq == packet.seq);
    assert(decoded.ttl == packet.ttl);
    assert(decoded_payload_len == payload_len);

    assert(tlv_find(decoded_payload, decoded_payload_len, TLV_EVENT_SEQ, &value, &value_len) == PROTO_OK);
    assert(value_len == 4u);
    assert(proto_get_u32_le(value) == 42u);

    assert(tlv_find(decoded_payload, decoded_payload_len, TLV_CLICKER_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 8u);
    assert(proto_get_u64_le(value) == 0x1122334455667788ull);
}

static void test_decode_rejects_bad_crc(void)
{
    uint8_t encoded[PACKET_MAX_LEN];
    size_t encoded_len = 0u;
    const uint8_t payload[] = {1u, 0u};

    struct proto_packet packet = {
        .msg_type = MSG_SELF_TEST_REPORT,
        .flags = FLAG_DIAGNOSTIC,
        .src_id = 1u,
        .dst_id = 2u,
        .session_id = 3u,
        .seq = 4u,
        .ttl = 1u,
        .payload_len = sizeof(payload),
    };

    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_OK);
    encoded[PACKET_HEADER_LEN] ^= 0x01u;

    struct proto_packet decoded = {0};
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    assert(proto_packet_decode(encoded, encoded_len, &decoded, &decoded_payload, &decoded_payload_len) == PROTO_ERR_BAD_CRC);
}

static void test_cobs_round_trip(void)
{
    const uint8_t raw[] = {0x11u, 0x00u, 0x22u, 0x33u, 0x00u, 0x00u, 0x44u};
    uint8_t encoded[32];
    uint8_t decoded[32];
    size_t encoded_len = 0u;
    size_t decoded_len = 0u;

    assert(proto_cobs_encode(raw, sizeof(raw), encoded, sizeof(encoded), &encoded_len) == PROTO_OK);
    for (size_t i = 0u; i < encoded_len; i++) {
        assert(encoded[i] != 0u);
    }
    assert(proto_cobs_decode(encoded, encoded_len, decoded, sizeof(decoded), &decoded_len) == PROTO_OK);
    assert(decoded_len == sizeof(raw));
    assert(memcmp(raw, decoded, sizeof(raw)) == 0);
}

int main(void)
{
    test_crc_known_vector();
    test_tlv_and_packet_round_trip();
    test_decode_rejects_bad_crc();
    test_cobs_round_trip();
    return 0;
}
