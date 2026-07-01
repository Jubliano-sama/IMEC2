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

static void assert_command_result_id_equal(const struct command_result_id *actual,
                                           const struct command_result_id *expected)
{
    assert(actual->gateway_id == expected->gateway_id);
    assert(actual->gateway_epoch == expected->gateway_epoch);
    assert(actual->command_seq == expected->command_seq);
    assert(actual->node_id == expected->node_id);
    assert(actual->node_boot_counter == expected->node_boot_counter);
    assert(actual->result_seq == expected->result_seq);
}

static void test_collection_control_packet_types_round_trip(void)
{
    const uint8_t payload[] = {
        TLV_GATEWAY_ID, 8u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
        TLV_COMMAND_SEQ, 4u, 9u, 0u, 0u, 0u,
    };
    const uint8_t msg_types[] = {
        MSG_RELAY_BUSY,
        MSG_RESULT_BUSY,
        MSG_RESULT_OFFER,
        MSG_RESULT_GRANT,
        MSG_RESULT_BUNDLE,
        MSG_GATEWAY_COLLECTION_EACK,
    };
    uint8_t encoded[PACKET_MAX_LEN];
    size_t encoded_len = 0u;
    const uint8_t *decoded_payload = NULL;
    size_t decoded_payload_len = 0u;

    for (size_t i = 0u; i < sizeof(msg_types); i++) {
        const struct proto_packet packet = {
            .msg_type = msg_types[i],
            .flags = FLAG_GATEWAY_ACK_REQUIRED,
            .src_id = 0x1111222233334444ull,
            .dst_id = 0x5555666677778888ull,
            .session_id = 0x01020304u,
            .seq = (uint16_t)(0x20u + i),
            .ttl = 4u,
            .payload_len = sizeof(payload),
            .message_age_ms = 15u,
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
        assert(decoded.msg_type == packet.msg_type);
        assert(decoded_payload_len == sizeof(payload));
        assert(memcmp(decoded_payload, payload, sizeof(payload)) == 0);
    }
}

static void test_command_result_id_tlvs_round_trip_and_require_identity(void)
{
    const struct command_result_id id = {
        .gateway_id = 0x0102030405060708ull,
        .gateway_epoch = 0x1122u,
        .command_seq = 0x33445566u,
        .node_id = 0x8877665544332211ull,
        .node_boot_counter = 0xAABBCCDDu,
        .result_seq = 0xEEFFu,
    };
    struct command_result_id decoded = {0};
    uint8_t payload[96];
    uint8_t missing[96];
    size_t payload_len = 0u;
    size_t missing_len = 0u;

    assert(command_result_id_append_tlvs(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         &id) == PROTO_OK);
    assert(command_result_id_from_tlvs(payload, payload_len, &decoded) == PROTO_OK);
    assert_command_result_id_equal(&decoded, &id);

    assert(tlv_append_u64(missing,
                          sizeof(missing),
                          &missing_len,
                          TLV_GATEWAY_ID,
                          id.gateway_id) == PROTO_OK);
    assert(tlv_append_u16(missing,
                          sizeof(missing),
                          &missing_len,
                          TLV_GATEWAY_EPOCH,
                          id.gateway_epoch) == PROTO_OK);
    assert(tlv_append_u32(missing,
                          sizeof(missing),
                          &missing_len,
                          TLV_COMMAND_SEQ,
                          id.command_seq) == PROTO_OK);
    assert(tlv_append_u64(missing,
                          sizeof(missing),
                          &missing_len,
                          TLV_NODE_ID,
                          id.node_id) == PROTO_OK);
    assert(command_result_id_from_tlvs(missing,
                                       missing_len,
                                       &decoded) == PROTO_ERR_NOT_FOUND);
}

static void test_result_offer_grant_busy_tlvs_round_trip(void)
{
    const struct command_result_id id = {
        .gateway_id = 0xAA00000000000001ull,
        .gateway_epoch = 7u,
        .command_seq = 0x12345678u,
        .node_id = 0xBB00000000000002ull,
        .node_boot_counter = 3u,
        .result_seq = 4u,
    };
    const struct result_offer offer = {
        .result_id = id,
        .result_len = 1023u,
        .result_crc = 0xBEEFu,
        .priority = 6u,
    };
    const struct result_grant grant = {
        .result_id = id,
        .granted_channel = 9u,
        .max_bytes = 512u,
        .event_offset_hint = 33u,
    };
    const struct result_busy busy = {
        .result_id = id,
        .retry_after_ms = 750u,
        .capacity_state = 2u,
        .optional_alternate_parent = 0xCC00000000000003ull,
        .has_optional_alternate_parent = true,
    };
    struct result_offer decoded_offer = {0};
    struct result_grant decoded_grant = {0};
    struct result_busy decoded_busy = {0};
    uint8_t payload[160];
    size_t payload_len = 0u;

    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    assert(result_offer_from_tlvs(payload, payload_len, &decoded_offer) == PROTO_OK);
    assert_command_result_id_equal(&decoded_offer.result_id, &id);
    assert(decoded_offer.result_len == offer.result_len);
    assert(decoded_offer.result_crc == offer.result_crc);
    assert(decoded_offer.priority == offer.priority);

    payload_len = 0u;
    assert(result_grant_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &grant) == PROTO_OK);
    assert(result_grant_from_tlvs(payload, payload_len, &decoded_grant) == PROTO_OK);
    assert_command_result_id_equal(&decoded_grant.result_id, &id);
    assert(decoded_grant.granted_channel == grant.granted_channel);
    assert(decoded_grant.max_bytes == grant.max_bytes);
    assert(decoded_grant.event_offset_hint == grant.event_offset_hint);

    payload_len = 0u;
    assert(result_busy_append_tlvs(payload,
                                   sizeof(payload),
                                   &payload_len,
                                   &busy) == PROTO_OK);
    assert(result_busy_from_tlvs(payload, payload_len, &decoded_busy) == PROTO_OK);
    assert_command_result_id_equal(&decoded_busy.result_id, &id);
    assert(decoded_busy.retry_after_ms == busy.retry_after_ms);
    assert(decoded_busy.capacity_state == busy.capacity_state);
    assert(decoded_busy.has_optional_alternate_parent);
    assert(decoded_busy.optional_alternate_parent == busy.optional_alternate_parent);
}

static void test_result_bundle_and_collection_eack_tlvs_round_trip(void)
{
    const struct result_bundle_header bundle = {
        .gateway_id = 0x1000000000000001ull,
        .gateway_epoch = 11u,
        .command_seq = 12u,
        .collection_epoch_id = 13u,
        .bundle_id = 14u,
        .record_count = 8u,
        .bundle_crc = 0xCAFEu,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = bundle.gateway_id,
        .gateway_epoch = bundle.gateway_epoch,
        .command_seq = bundle.command_seq,
        .collection_epoch_id = bundle.collection_epoch_id,
        .membership_epoch = 15u,
        .expected_count = 16u,
        .received_count = 10u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 2u,
        .next_retry_spread_ms = 30000u,
        .collection_open = true,
    };
    struct result_bundle_header decoded_bundle = {0};
    struct gateway_collection_eack decoded_eack = {0};
    uint8_t payload[160];
    size_t payload_len = 0u;

    assert(result_bundle_header_append_tlvs(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &bundle) == PROTO_OK);
    assert(result_bundle_header_from_tlvs(payload,
                                          payload_len,
                                          &decoded_bundle) == PROTO_OK);
    assert(decoded_bundle.gateway_id == bundle.gateway_id);
    assert(decoded_bundle.gateway_epoch == bundle.gateway_epoch);
    assert(decoded_bundle.command_seq == bundle.command_seq);
    assert(decoded_bundle.collection_epoch_id == bundle.collection_epoch_id);
    assert(decoded_bundle.bundle_id == bundle.bundle_id);
    assert(decoded_bundle.record_count == bundle.record_count);
    assert(decoded_bundle.bundle_crc == bundle.bundle_crc);

    payload_len = 0u;
    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    assert(gateway_collection_eack_from_tlvs(payload,
                                             payload_len,
                                             &decoded_eack) == PROTO_OK);
    assert(decoded_eack.gateway_id == eack.gateway_id);
    assert(decoded_eack.gateway_epoch == eack.gateway_epoch);
    assert(decoded_eack.command_seq == eack.command_seq);
    assert(decoded_eack.collection_epoch_id == eack.collection_epoch_id);
    assert(decoded_eack.membership_epoch == eack.membership_epoch);
    assert(decoded_eack.expected_count == eack.expected_count);
    assert(decoded_eack.received_count == eack.received_count);
    assert(decoded_eack.eack_format == eack.eack_format);
    assert(decoded_eack.retry_round == eack.retry_round);
    assert(decoded_eack.next_retry_spread_ms == eack.next_retry_spread_ms);
    assert(decoded_eack.collection_open == eack.collection_open);

    payload[payload_len - 1u] = 2u;
    assert(gateway_collection_eack_from_tlvs(payload,
                                             payload_len,
                                             &decoded_eack) == PROTO_ERR_MALFORMED);
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
    test_collection_control_packet_types_round_trip();
    test_command_result_id_tlvs_round_trip_and_require_identity();
    test_result_offer_grant_busy_tlvs_round_trip();
    test_result_bundle_and_collection_eack_tlvs_round_trip();
    test_cobs_round_trip();
    return 0;
}
