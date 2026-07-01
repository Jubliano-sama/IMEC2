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
        .message_age_ms = 123456u,
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
    assert(decoded.message_age_ms == packet.message_age_ms);
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

static void test_extended_packet_round_trip(void)
{
    uint8_t payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    uint8_t encoded[PACKET_EXT_MAX_LEN];
    size_t encoded_len = 0u;
    struct proto_packet decoded = {0};
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;

    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i ^ (i >> 3));
    }

    const struct proto_packet packet = {
        .msg_type = MSG_MESH_DATA,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = 0x1122334455667788ull,
        .dst_id = 0x9999888877776666ull,
        .session_id = 0xABCDEF01u,
        .seq = 0x1234u,
        .ttl = 7u,
        .payload_len = sizeof(payload),
        .message_age_ms = 0x01020304u,
    };

    assert(proto_packet_header_len(254u) == PACKET_HEADER_LEN);
    assert(proto_packet_header_len(255u) == PACKET_EXT_HEADER_LEN);
    assert(proto_packet_encoded_len(packet.payload_len) == PACKET_EXT_MAX_LEN);
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_OK);
    assert(encoded_len == PACKET_EXT_MAX_LEN);
    assert(encoded[27] == PACKET_EXT_LENGTH_SENTINEL);
    assert(proto_get_u16_le(&encoded[28]) == packet.payload_len);
    assert(proto_get_u32_le(&encoded[30]) == packet.message_age_ms);

    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_OK);
    assert(decoded.msg_type == packet.msg_type);
    assert(decoded.flags == packet.flags);
    assert(decoded.src_id == packet.src_id);
    assert(decoded.dst_id == packet.dst_id);
    assert(decoded.session_id == packet.session_id);
    assert(decoded.seq == packet.seq);
    assert(decoded.ttl == packet.ttl);
    assert(decoded.payload_len == packet.payload_len);
    assert(decoded.message_age_ms == packet.message_age_ms);
    assert(decoded_payload_len == sizeof(payload));
    assert(memcmp(decoded_payload, payload, sizeof(payload)) == 0);
}

static void set_packet_type_and_refresh_crc(uint8_t *encoded, size_t encoded_len, uint8_t msg_type)
{
    encoded[2] = msg_type;
    proto_put_u16_le(&encoded[encoded_len - PACKET_CRC_LEN],
                     proto_crc16_ccitt_false(encoded, encoded_len - PACKET_CRC_LEN));
}

static void test_packet_rejects_retired_and_compact_only_message_types(void)
{
    uint8_t encoded[PACKET_MAX_LEN];
    size_t encoded_len = 0u;
    const uint8_t payload[] = {TLV_EVENT_SEQ, 4u, 1u, 0u, 0u, 0u};
    struct proto_packet packet = {
        .msg_type = MSG_CLICK_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = 1u,
        .dst_id = 2u,
        .session_id = 3u,
        .seq = 4u,
        .ttl = 1u,
        .payload_len = sizeof(payload),
    };
    struct proto_packet decoded = {0};
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;

    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_OK);

    packet.msg_type = 0x01u;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = 0x02u;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = MSG_UWB_WAKE_CLAIM;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = MSG_UWB_RANGE_RELEASE;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = MSG_UWB_POLL;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = MSG_UWB_CLICKER_DIAG;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = 0x33u;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);
    packet.msg_type = 0x34u;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_ERR_MALFORMED);

    set_packet_type_and_refresh_crc(encoded, encoded_len, 0x01u);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_ERR_MALFORMED);

    set_packet_type_and_refresh_crc(encoded, encoded_len, MSG_UWB_RANGE_SCHEDULE);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_ERR_MALFORMED);

    set_packet_type_and_refresh_crc(encoded, encoded_len, MSG_UWB_RANGE_RELEASE);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_ERR_MALFORMED);

    set_packet_type_and_refresh_crc(encoded, encoded_len, MSG_UWB_CLICKER_DIAG);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_ERR_MALFORMED);

    set_packet_type_and_refresh_crc(encoded, encoded_len, 0x33u);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_ERR_MALFORMED);
}

static void test_mesh_event_packet_types_round_trip(void)
{
    const uint8_t payload[] = {
        TLV_MESH_CHANNEL, 1u, 9u,
        TLV_MESH_EVENT_WINDOW_MS, 2u, 20u, 0u,
    };
    uint8_t encoded[PACKET_MAX_LEN];
    size_t encoded_len = 0u;
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;
    const uint8_t event_types[] = {
        MSG_MESH_EVENT_PROPOSE,
        MSG_MESH_EVENT_ACCEPT,
        MSG_MESH_EVENT_UPDATE,
        MSG_MESH_EVENT_END,
    };

    for (size_t i = 0u; i < sizeof(event_types); i++) {
        struct proto_packet packet = {
            .msg_type = event_types[i],
            .src_id = 1u,
            .dst_id = 2u,
            .session_id = 3u,
            .seq = (uint16_t)(4u + i),
            .ttl = 1u,
            .payload_len = sizeof(payload),
        };
        struct proto_packet decoded = {0};

        assert(proto_packet_encode(&packet,
                                   payload,
                                   encoded,
                                   sizeof(encoded),
                                   &encoded_len) == PROTO_OK);
        assert(proto_packet_decode(encoded,
                                   encoded_len,
                                   &decoded,
                                   &decoded_payload,
                                   &decoded_payload_len) == PROTO_OK);
        assert(decoded.msg_type == event_types[i]);
        assert(decoded_payload_len == sizeof(payload));
        assert(memcmp(decoded_payload, payload, sizeof(payload)) == 0);
    }
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
    test_extended_packet_round_trip();
    test_packet_rejects_retired_and_compact_only_message_types();
    test_mesh_event_packet_types_round_trip();
    test_cobs_round_trip();
    return 0;
}
