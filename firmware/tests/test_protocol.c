#include "protocol.h"

#include <assert.h>
#include <string.h>

static void test_crc_known_vector(void)
{
    const uint8_t data[] = "123456789";
    assert(proto_crc16_ccitt_false(data, 9u) == 0x29B1u);
}

static void test_click_report_transport_session_identity(void)
{
    const uint64_t first_clicker = UINT64_C(0xaaaabbbbccccdddd);
    const uint64_t second_clicker = UINT64_C(0xaaaabbbbccccddde);
    const uint32_t event_seq = UINT32_C(7);
    const uint32_t first_session =
        proto_click_report_session_id(first_clicker, event_seq);
    const uint32_t second_session =
        proto_click_report_session_id(second_clicker, event_seq);

    /* Fixed vectors keep the C and Python implementations byte-for-byte tied. */
    assert(proto_click_report_session_id(
               UINT64_C(0x1111222233334444), UINT32_C(0x11223344)) ==
           UINT32_C(0x1bcf6ce5));
    assert(first_session == UINT32_C(0xb0cac892));
    assert(second_session == UINT32_C(0x598766a9));
    assert(first_session != second_session);
    assert(proto_click_report_session_id(first_clicker, event_seq) ==
           first_session);
    assert(first_session != 0u && second_session != 0u);
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
    packet.msg_type = MSG_GATEWAY_ACK_CONFIRM;
    assert(proto_packet_encode(&packet, payload, encoded, sizeof(encoded), &encoded_len) == PROTO_OK);
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

    set_packet_type_and_refresh_crc(encoded,
                                    encoded_len,
                                    MSG_GATEWAY_ACK_CONFIRM);
    assert(proto_packet_decode(encoded,
                               encoded_len,
                               &decoded,
                               &decoded_payload,
                               &decoded_payload_len) == PROTO_OK);
    assert(decoded.msg_type == MSG_GATEWAY_ACK_CONFIRM);

    set_packet_type_and_refresh_crc(encoded, encoded_len, 0x34u);
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
        MSG_GATEWAY_COMMAND_EVENT,
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
        .result_digest = {
            0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
            0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du, 0x0eu, 0x0fu,
            0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
            0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1du, 0x1eu, 0x1fu,
        },
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
        .capacity_validity_interval_ms = 1250u,
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
    assert(payload_len == RESULT_OFFER_TLV_BYTES);
    assert(result_offer_from_tlvs(payload, payload_len, &decoded_offer) == PROTO_OK);
    assert_command_result_id_equal(&decoded_offer.result_id, &id);
    assert(decoded_offer.result_len == offer.result_len);
    assert(decoded_offer.result_crc == offer.result_crc);
    assert(memcmp(decoded_offer.result_digest,
                  offer.result_digest,
                  sizeof(offer.result_digest)) == 0);
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
    assert(decoded_busy.capacity_validity_interval_ms == busy.capacity_validity_interval_ms);
    assert(decoded_busy.has_optional_alternate_parent);
    assert(decoded_busy.optional_alternate_parent == busy.optional_alternate_parent);
}

static void test_result_offer_requires_full_semantic_commitment(void)
{
    const struct result_offer offer = {
        .result_id = {
            .gateway_id = 0x1000000000000001ull,
            .gateway_epoch = 2u,
            .command_seq = 3u,
            .node_id = 0x2000000000000001ull,
            .node_boot_counter = 4u,
            .result_seq = 5u,
        },
        .result_len = 64u,
        .result_crc = 0x1234u,
        .result_digest = {0xa5u},
        .priority = 1u,
    };
    struct result_offer decoded;
    uint8_t payload[160];
    uint8_t too_small[RESULT_OFFER_TLV_BYTES - 1u];
    const uint8_t *digest_value = NULL;
    uint8_t digest_len = 0u;
    size_t payload_len = 0u;

    assert(result_offer_from_tlvs(NULL, 0u, &decoded) == PROTO_ERR_ARG);
    assert(result_offer_append_tlvs(NULL,
                                    RESULT_OFFER_TLV_BYTES,
                                    &payload_len,
                                    &offer) == PROTO_ERR_ARG);
    assert(result_offer_append_tlvs(too_small,
                                    sizeof(too_small),
                                    &payload_len,
                                    &offer) == PROTO_ERR_NO_SPACE);
    assert(payload_len == 0u);

    assert(command_result_id_append_tlvs(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         &offer.result_id) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_PAYLOAD_LEN,
                          offer.result_len) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_PAYLOAD_CRC,
                          offer.result_crc) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_PRIORITY,
                         offer.priority) == PROTO_OK);
    assert(result_offer_from_tlvs(payload,
                                  payload_len,
                                  &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    assert(tlv_find_unique(payload,
                           payload_len,
                           TLV_RESULT_SHA256_COMMITMENT,
                           &digest_value,
                           &digest_len) == PROTO_OK);
    assert(digest_len == SEMANTIC_DIGEST_SHA256_LEN);
    payload[(size_t)(digest_value - payload) - 1u] =
        SEMANTIC_DIGEST_SHA256_LEN - 1u;
    assert(result_offer_from_tlvs(payload,
                                  payload_len,
                                  &decoded) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_offer_append_tlvs(payload,
                                    sizeof(payload),
                                    &payload_len,
                                    &offer) == PROTO_OK);
    assert(tlv_append_bytes(payload,
                            sizeof(payload),
                            &payload_len,
                            TLV_RESULT_SHA256_COMMITMENT,
                            offer.result_digest,
                            sizeof(offer.result_digest)) == PROTO_OK);
    assert(result_offer_from_tlvs(payload,
                                  payload_len,
                                  &decoded) == PROTO_ERR_MALFORMED);
}

static void test_result_bundle_and_collection_eack_tlvs_round_trip(void)
{
    const struct result_bundle_header bundle = {
        .gateway_id = 0x1000000000000001ull,
        .gateway_epoch = 11u,
        .command_seq = 12u,
        .collection_epoch_id = 13u,
        .bundle_id = 14u,
        .record_count = 1u,
        .bundle_crc = 0xCAFEu,
    };
    const struct command_result_id result_id = {
        .gateway_id = bundle.gateway_id,
        .gateway_epoch = bundle.gateway_epoch,
        .command_seq = bundle.command_seq,
        .node_id = 0x2222333344445555ull,
        .node_boot_counter = 3u,
        .result_seq = 4u,
    };
    const uint8_t record_payload[] = {0x10u, 0x20u, 0x30u, 0x40u};
    const struct gateway_collection_eack eack = {
        .gateway_id = bundle.gateway_id,
        .gateway_epoch = bundle.gateway_epoch,
        .command_seq = bundle.command_seq,
        .collection_epoch_id = bundle.collection_epoch_id,
        .membership_epoch = 15u,
        .expected_count = 12u,
        .received_count = 10u,
        .packet_sequence = 513u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 2u,
        .next_retry_spread_ms = 30000u,
        .collection_open = true,
    };
    const struct result_bundle_record record = {
        .result_id = result_id,
        .payload_len = sizeof(record_payload),
        .payload_crc = proto_crc16_ccitt_false(record_payload, sizeof(record_payload)),
        .payload = record_payload,
    };
    struct result_bundle_header decoded_bundle = {0};
    struct result_bundle_record decoded_record = {0};
    struct gateway_collection_eack decoded_eack = {0};
    struct proto_packet eack_packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = bundle.gateway_id,
        .dst_id = 0u,
        .session_id = bundle.command_seq,
        .seq = eack.packet_sequence,
        .ttl = 3u,
    };
    uint8_t payload[160];
    size_t payload_len = 0u;
    size_t cursor = 0u;

    assert(result_bundle_header_append_tlvs(payload,
                                            sizeof(payload),
                                            &payload_len,
                                            &bundle) == PROTO_OK);
    assert(payload_len == RESULT_BUNDLE_HEADER_TLV_BYTES);
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
    assert(result_bundle_record_append_tlv(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           &record) == PROTO_OK);
    assert(result_bundle_record_next_from_tlvs(payload,
                                               payload_len,
                                               &cursor,
                                               &decoded_record) == PROTO_OK);
    assert(decoded_record.result_id.gateway_id == result_id.gateway_id);
    assert(decoded_record.result_id.gateway_epoch == result_id.gateway_epoch);
    assert(decoded_record.result_id.command_seq == result_id.command_seq);
    assert(decoded_record.result_id.node_id == result_id.node_id);
    assert(decoded_record.result_id.node_boot_counter == result_id.node_boot_counter);
    assert(decoded_record.result_id.result_seq == result_id.result_seq);
    assert(decoded_record.payload_len == sizeof(record_payload));
    assert(decoded_record.payload_crc == record.payload_crc);
    assert(memcmp(decoded_record.payload,
                  record_payload,
                  sizeof(record_payload)) == 0);
    assert(result_bundle_record_next_from_tlvs(payload,
                                               payload_len,
                                               &cursor,
                                               &decoded_record) == PROTO_ERR_NOT_FOUND);

    payload[payload_len - 1u] ^= 0x01u;
    cursor = 0u;
    assert(result_bundle_record_next_from_tlvs(payload,
                                               payload_len,
                                               &cursor,
                                               &decoded_record) == PROTO_ERR_BAD_CRC);

    payload_len = 0u;
    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          0x1001ull) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          0x1002ull) == PROTO_OK);
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
    assert(decoded_eack.packet_sequence == eack.packet_sequence);
    assert(decoded_eack.eack_format == eack.eack_format);
    assert(decoded_eack.retry_round == eack.retry_round);
    assert(decoded_eack.next_retry_spread_ms == eack.next_retry_spread_ms);
    assert(decoded_eack.collection_open == eack.collection_open);
    eack_packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_eack_packet_validate(&eack_packet,
                                                   payload,
                                                   payload_len,
                                                   &decoded_eack) == PROTO_OK);
    eack_packet.dst_id = 0x1002ull;
    assert(gateway_collection_eack_packet_validate(&eack_packet,
                                                   payload,
                                                   payload_len,
                                                   &decoded_eack) == PROTO_ERR_MALFORMED);
    eack_packet.dst_id = 0u;
    bool listed = false;
    assert(gateway_collection_eack_contains_node_id(payload,
                                                    payload_len,
                                                    0x1002ull,
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(payload,
                                                    payload_len,
                                                    0x9999ull,
                                                    &listed) == PROTO_OK);
    assert(!listed);

    payload_len = 0u;
    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    payload[payload_len - 1u] = 2u;
    assert(gateway_collection_eack_from_tlvs(payload,
                                             payload_len,
                                             &decoded_eack) == PROTO_ERR_MALFORMED);
    payload_len = 0u;
    assert(gateway_collection_eack_append_tlvs(payload,
                                               sizeof(payload),
                                               &payload_len,
                                               &eack) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_NODE_ID,
                         0x12u) == PROTO_OK);
    assert(gateway_collection_eack_contains_node_id(payload,
                                                    payload_len,
                                                    0x12u,
                                                    &listed) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          0x12u) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_NODE_ID,
                         0x34u) == PROTO_OK);
    assert(gateway_collection_eack_contains_node_id(payload,
                                                    payload_len,
                                                    0x12u,
                                                    &listed) == PROTO_ERR_MALFORMED);
}

static void test_collection_eack_rejects_ambiguous_lists_and_schema_smuggling(void)
{
    struct gateway_collection_eack eack = {
        .gateway_id = UINT64_C(0x1000000000000001),
        .gateway_epoch = 2u,
        .command_seq = 3u,
        .collection_epoch_id = 4u,
        .membership_epoch = 5u,
        .expected_count = 2u,
        .received_count = 1u,
        .packet_sequence = 6u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = 100u,
        .collection_open = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_COLLECTION_EACK,
        .src_id = UINT64_C(0x1000000000000001),
        .dst_id = 0u,
        .session_id = 3u,
        .seq = 6u,
        .ttl = 1u,
    };
    uint8_t payload[PROTO_GATEWAY_COLLECTION_EACK_MAX_PAYLOAD_LEN];
    size_t fixed_len = 0u;
    size_t payload_len;

    assert(gateway_collection_eack_append_tlvs(
               payload, sizeof(payload), &fixed_len, &eack) == PROTO_OK);
    packet.payload_len = (uint16_t)fixed_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, fixed_len, NULL) == PROTO_ERR_MALFORMED);

    payload_len = fixed_len;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          UINT64_C(0x2001)) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, payload_len, NULL) == PROTO_OK);

    packet.flags = FLAG_DIAGNOSTIC;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, payload_len, NULL) == PROTO_ERR_MALFORMED);
    packet.flags = 0u;

    payload_len = fixed_len;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          0u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, payload_len, NULL) == PROTO_ERR_MALFORMED);

    eack.expected_count = 3u;
    fixed_len = 0u;
    assert(gateway_collection_eack_append_tlvs(
               payload, sizeof(payload), &fixed_len, &eack) == PROTO_OK);
    payload_len = fixed_len;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          UINT64_C(0x2001)) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          UINT64_C(0x2001)) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, payload_len, NULL) == PROTO_ERR_MALFORMED);

    eack.expected_count = 2u;
    fixed_len = 0u;
    assert(gateway_collection_eack_append_tlvs(
               payload, sizeof(payload), &fixed_len, &eack) == PROTO_OK);
    payload_len = fixed_len;
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_NODE_ID,
                          UINT64_C(0x2001)) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_ERROR_DETAIL,
                         0x55u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, payload_len, NULL) == PROTO_ERR_MALFORMED);

    eack.eack_format = EACK_FORMAT_ROSTER_BITMAP;
    fixed_len = 0u;
    assert(gateway_collection_eack_append_tlvs(
               payload, sizeof(payload), &fixed_len, &eack) == PROTO_OK);
    packet.payload_len = (uint16_t)fixed_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, fixed_len, NULL) == PROTO_ERR_MALFORMED);

    eack.eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST;
    eack.expected_count = PROTO_GATEWAY_COLLECTION_EACK_NODE_CAP + 1u;
    fixed_len = 0u;
    assert(gateway_collection_eack_append_tlvs(
               payload, sizeof(payload), &fixed_len, &eack) == PROTO_OK);
    packet.payload_len = (uint16_t)fixed_len;
    assert(gateway_collection_eack_packet_validate(
               &packet, payload, fixed_len, NULL) == PROTO_ERR_MALFORMED);
}

static void test_unique_tlv_and_result_decoders_reject_duplicates(void)
{
    const struct command_result_id result_id = {
        .gateway_id = 0x1000000000000001ull,
        .gateway_epoch = 2u,
        .command_seq = 3u,
        .node_id = 0x2000000000000001ull,
        .node_boot_counter = 4u,
        .result_seq = 5u,
    };
    const struct result_offer offer = {
        .result_id = result_id,
        .result_len = 64u,
        .result_crc = 0x1234u,
        .priority = 1u,
    };
    const struct result_grant grant = {
        .result_id = result_id,
        .granted_channel = 9u,
        .max_bytes = 255u,
        .event_offset_hint = 10u,
    };
    const struct result_busy busy = {
        .result_id = result_id,
        .retry_after_ms = 25u,
        .capacity_state = 1u,
        .capacity_validity_interval_ms = 50u,
        .optional_alternate_parent = 0x3000000000000001ull,
        .has_optional_alternate_parent = true,
    };
    const struct result_bundle_header bundle = {
        .gateway_id = result_id.gateway_id,
        .gateway_epoch = result_id.gateway_epoch,
        .command_seq = result_id.command_seq,
        .collection_epoch_id = 6u,
        .bundle_id = 7u,
        .record_count = 1u,
        .bundle_crc = 0x5678u,
    };
    const struct gateway_collection_eack eack = {
        .gateway_id = result_id.gateway_id,
        .gateway_epoch = result_id.gateway_epoch,
        .command_seq = result_id.command_seq,
        .collection_epoch_id = 6u,
        .membership_epoch = 7u,
        .expected_count = 2u,
        .received_count = 1u,
        .packet_sequence = 8u,
        .eack_format = EACK_FORMAT_EXPLICIT_MISSING_LIST,
        .retry_round = 1u,
        .next_retry_spread_ms = 100u,
        .collection_open = true,
    };
    struct command_result_id decoded_id;
    struct result_offer decoded_offer;
    struct result_grant decoded_grant;
    struct result_busy decoded_busy;
    struct result_bundle_header decoded_bundle;
    struct gateway_collection_eack decoded_eack;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t payload[256];
    size_t payload_len = 0u;

    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_SEQ, 1u) == PROTO_OK);
    assert(tlv_find_unique(payload, payload_len, TLV_COMMAND_SEQ,
                           &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    assert(proto_get_u32_le(value) == 1u);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_SEQ, 2u) == PROTO_OK);
    assert(tlv_find_unique(payload, payload_len, TLV_COMMAND_SEQ,
                           &value, &value_len) == PROTO_ERR_MALFORMED);
    payload_len = PROTO_TLV_U32_ENCODED_LEN;
    payload[payload_len++] = TLV_REASON;
    assert(tlv_find_unique(payload, payload_len, TLV_COMMAND_SEQ,
                           &value, &value_len) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(command_result_id_append_tlvs(payload, sizeof(payload),
                                         &payload_len, &result_id) == PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_GATEWAY_ID,
                          result_id.gateway_id + 1u) == PROTO_OK);
    assert(command_result_id_from_tlvs(payload, payload_len,
                                       &decoded_id) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_offer_append_tlvs(payload, sizeof(payload),
                                    &payload_len, &offer) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_PRIORITY, offer.priority + 1u) == PROTO_OK);
    assert(result_offer_from_tlvs(payload, payload_len,
                                  &decoded_offer) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_grant_append_tlvs(payload, sizeof(payload),
                                    &payload_len, &grant) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_MAX_BYTES, grant.max_bytes + 1u) == PROTO_OK);
    assert(result_grant_from_tlvs(payload, payload_len,
                                  &decoded_grant) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_busy_append_tlvs(payload, sizeof(payload),
                                   &payload_len, &busy) == PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_ALTERNATE_PARENT_ID,
                          busy.optional_alternate_parent + 1u) == PROTO_OK);
    assert(result_busy_from_tlvs(payload, payload_len,
                                 &decoded_busy) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(result_bundle_header_append_tlvs(payload, sizeof(payload),
                                            &payload_len, &bundle) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_BUNDLE_CRC, bundle.bundle_crc + 1u) == PROTO_OK);
    assert(result_bundle_header_from_tlvs(payload, payload_len,
                                          &decoded_bundle) == PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(gateway_collection_eack_append_tlvs(payload, sizeof(payload),
                                               &payload_len, &eack) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_COLLECTION_OPEN, 0u) == PROTO_OK);
    assert(gateway_collection_eack_from_tlvs(payload, payload_len,
                                             &decoded_eack) == PROTO_ERR_MALFORMED);
}

static void test_report_validators_reject_schema_smuggling_and_mismatch(void)
{
    const uint64_t source_id = UINT64_C(0x1122334455667788);
    const uint64_t gateway_id = UINT64_C(0x8877665544332211);
    struct proto_packet packet = {
        .msg_type = MSG_SELF_TEST_REPORT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = UINT32_C(0x00010001),
        .seq = 1u,
        .ttl = 4u,
    };
    uint8_t payload[192];
    size_t payload_len = 0u;

    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_CLICKER_ID, source_id) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_EVENT_SEQ, packet.session_id) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_ERROR_CODE, 6u) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_BATTERY_MV, 3000u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(proto_self_test_report_validate(&packet, payload, payload_len) ==
           PROTO_OK);

    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_REASON, 1u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(proto_self_test_report_validate(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);
    payload_len -= PROTO_TLV_U8_ENCODED_LEN;
    packet.payload_len = (uint16_t)payload_len;
    packet.seq = 2u;
    assert(proto_self_test_report_validate(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);

    packet = (struct proto_packet) {
        .msg_type = MSG_ANCHOR_HEARTBEAT,
        .flags = FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = source_id,
        .dst_id = gateway_id,
        .session_id = 7u,
        .seq = 8u,
        .ttl = 4u,
    };
    payload_len = 0u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DEVICE_ROLE, ROLE_ANCHOR) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_BATTERY_MV, 0u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_STATUS_BITS, 0u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_UPTIME_MS, 100u) == PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_TIMESTAMP_MS, 101u) == PROTO_OK);
    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_GATEWAY_ID, gateway_id) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_REASON, (uint8_t)(-PROTO_ERR_NOT_FOUND)) ==
           PROTO_OK);
    {
        static const uint8_t telemetry_types[] = {
            TLV_MESH_DUPLICATE_COUNT,
            TLV_COLLECTION_PENDING_COUNT,
            TLV_PARENT_HOLDDOWN_COUNT,
            TLV_ROUTE_DISCOVERY_ATTEMPTS,
            TLV_OUTBOX_DELIVERY_STATE,
            TLV_FLOOD_SUPPRESSION_COUNT,
            TLV_ROUTE_REPLY_RETRY_COUNT,
            TLV_BUSY_RESPONSE_COUNT,
        };
        static const uint8_t metric_types[] = {
            TLV_MESH_CHANNEL_SWITCHES,
            TLV_MESH_PLL_READY_FAILURES,
            TLV_MESH_LATE_CHANNEL5_RETURNS,
            TLV_MESH_DEFERRALS,
            TLV_MESH_CH9_EVENT_MISSES,
            TLV_MESH_CHANNEL5_PREEMPTIONS,
            TLV_MESH_CH9_REPORT_LATENCY_MS,
        };

        for (size_t i = 0u;
             i < sizeof(telemetry_types) / sizeof(telemetry_types[0]);
             i++) {
            assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                                 telemetry_types[i], 0u) == PROTO_OK);
        }
        for (size_t i = 0u;
             i < sizeof(metric_types) / sizeof(metric_types[0]);
             i++) {
            assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                                  metric_types[i], 0u) == PROTO_OK);
        }
    }
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_DISCOVERY_ASSIGNMENT_EPOCH, 1u) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(proto_anchor_heartbeat_validate(&packet, payload, payload_len) ==
           PROTO_OK);

    assert(tlv_append_u64(payload, sizeof(payload), &payload_len,
                          TLV_GATEWAY_ID, gateway_id) == PROTO_OK);
    packet.payload_len = (uint16_t)payload_len;
    assert(proto_anchor_heartbeat_validate(&packet, payload, payload_len) ==
           PROTO_ERR_MALFORMED);
}

static void test_result_busy_disambiguates_alternate_from_correlation(void)
{
    const struct result_busy busy = {
        .result_id = {
            .gateway_id = UINT64_C(0x1010101010101010),
            .gateway_epoch = 3u,
            .command_seq = 4u,
            .node_id = UINT64_C(0x2020202020202020),
            .node_boot_counter = 5u,
            .result_seq = 6u,
        },
        .retry_after_ms = 100u,
        .capacity_state = 2u,
        .capacity_validity_interval_ms = 120u,
        .optional_alternate_parent = UINT64_C(0x3030303030303030),
        .has_optional_alternate_parent = true,
    };
    struct result_busy decoded = {0};
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(RESULT_BUSY_WITH_ALTERNATE_TLV_BYTES ==
           RESULT_BUSY_CORRELATED_TLV_BYTES);
    assert(result_busy_append_tlvs(payload, sizeof(payload), &payload_len,
                                   &busy) == PROTO_OK);
    assert(payload_len == RESULT_BUSY_WITH_ALTERNATE_TLV_BYTES);
    assert(result_busy_from_tlvs(payload, payload_len, &decoded) == PROTO_OK);
    assert(decoded.has_optional_alternate_parent);
    assert(decoded.optional_alternate_parent ==
           busy.optional_alternate_parent);

    payload_len = 0u;
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_REQUESTED_MSG_SESSION_ID, 77u) == PROTO_OK);
    assert(tlv_append_u16(payload, sizeof(payload), &payload_len,
                          TLV_REQUESTED_MSG_SEQ, 8u) == PROTO_OK);
    {
        struct result_busy correlated = busy;

        correlated.has_optional_alternate_parent = false;
        assert(result_busy_append_tlvs(payload, sizeof(payload), &payload_len,
                                       &correlated) == PROTO_OK);
    }
    assert(payload_len == RESULT_BUSY_CORRELATED_TLV_BYTES);
    assert(result_busy_from_tlvs(payload, payload_len, &decoded) == PROTO_OK);
    assert(!decoded.has_optional_alternate_parent);
}

static void test_gateway_host_receipt_identity_round_trip_and_strictness(void)
{
    const struct gateway_host_receipt_identity identity = {
        .original_msg_type = MSG_CLICK_REPORT,
        .original_flags = FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = UINT64_C(0x1122334455667788),
        .dst_id = UINT64_C(0x99AABBCCDDEEFF00),
        .session_id = UINT32_C(0xA1B2C3D4),
        .seq = UINT16_C(0xE5F6),
        .stream_record_digest = {
            0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
            0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
            0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
            0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu,
        },
    };
    static const uint8_t expected_value[PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN] = {
        0x20u, 0x24u,
        0x88u, 0x77u, 0x66u, 0x55u, 0x44u, 0x33u, 0x22u, 0x11u,
        0x00u, 0xFFu, 0xEEu, 0xDDu, 0xCCu, 0xBBu, 0xAAu, 0x99u,
        0xD4u, 0xC3u, 0xB2u, 0xA1u,
        0xF6u, 0xE5u,
        0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
        0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
        0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
        0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu,
    };
    uint8_t value[PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN];
    uint8_t payload[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES];
    struct gateway_host_receipt_identity decoded = {0};
    struct proto_packet packet = {
        .msg_type = MSG_GATEWAY_HOST_RECEIPT,
        .flags = 0u,
        .src_id = UINT64_C(0xA1C1BEEFC0DE0001),
        .dst_id = UINT64_C(0x9999888877776666),
        .session_id = identity.session_id,
        .seq = identity.seq,
        .ttl = 1u,
        .payload_len = PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES,
    };
    size_t value_len = 0u;
    size_t payload_len = 0u;

    assert(PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN == 56u);
    assert(PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES == 58u);
    assert(!proto_packet_msg_type_allowed_over_uwb(MSG_GATEWAY_HOST_RECEIPT));
    assert(gateway_host_receipt_identity_encode(&identity,
                                                value,
                                                sizeof(value),
                                                &value_len) == PROTO_OK);
    assert(value_len == sizeof(expected_value));
    assert(memcmp(value, expected_value, sizeof(value)) == 0);
    assert(gateway_host_receipt_identity_decode(value,
                                                value_len,
                                                &decoded) == PROTO_OK);
    assert(decoded.original_msg_type == identity.original_msg_type);
    assert(decoded.original_flags == identity.original_flags);
    assert(decoded.src_id == identity.src_id);
    assert(decoded.dst_id == identity.dst_id);
    assert(decoded.session_id == identity.session_id);
    assert(decoded.seq == identity.seq);
    assert(memcmp(decoded.stream_record_digest,
                  identity.stream_record_digest,
                  sizeof(decoded.stream_record_digest)) == 0);

    assert(gateway_host_receipt_identity_append_tlv(payload,
                                                     sizeof(payload),
                                                     &payload_len,
                                                     &identity) == PROTO_OK);
    assert(payload_len == sizeof(payload));
    assert(payload[0] == TLV_GATEWAY_HOST_RECEIPT_IDENTITY);
    assert(payload[1] == PROTO_GATEWAY_HOST_RECEIPT_IDENTITY_VALUE_LEN);
    assert(gateway_host_receipt_identity_from_tlvs(payload,
                                                   payload_len,
                                                   &decoded) == PROTO_OK);
    assert(decoded.original_msg_type == identity.original_msg_type);
    assert(decoded.original_flags == identity.original_flags);
    assert(decoded.src_id == identity.src_id);
    assert(decoded.dst_id == identity.dst_id);
    assert(decoded.session_id == identity.session_id);
    assert(decoded.seq == identity.seq);
    assert(memcmp(decoded.stream_record_digest,
                  identity.stream_record_digest,
                  sizeof(decoded.stream_record_digest)) == 0);
    assert(gateway_host_receipt_packet_validate(&packet,
                                                payload,
                                                payload_len,
                                                &decoded) == PROTO_OK);

    {
        uint8_t malformed[PROTO_GATEWAY_HOST_RECEIPT_TLV_BYTES + 2u];

        memcpy(malformed, payload, sizeof(payload));
        malformed[1] = (uint8_t)(malformed[1] - 1u);
        assert(gateway_host_receipt_identity_from_tlvs(malformed,
                                                       sizeof(payload),
                                                       &decoded) == PROTO_ERR_MALFORMED);

        memcpy(malformed, payload, sizeof(payload));
        malformed[sizeof(payload)] = TLV_GATEWAY_HOST_RECEIPT_IDENTITY;
        malformed[sizeof(payload) + 1u] = 0u;
        assert(gateway_host_receipt_identity_from_tlvs(malformed,
                                                       sizeof(malformed),
                                                       &decoded) == PROTO_ERR_MALFORMED);

        memcpy(malformed, payload, sizeof(payload));
        memset(&malformed[PROTO_TLV_HEADER_LEN + 2u],
               0,
               sizeof(identity.src_id));
        assert(gateway_host_receipt_identity_from_tlvs(malformed,
                                                       sizeof(payload),
                                                       &decoded) == PROTO_ERR_MALFORMED);
    }

    {
        struct gateway_host_receipt_identity invalid = identity;

        invalid.src_id = 0u;
        assert(gateway_host_receipt_identity_encode(&invalid,
                                                    value,
                                                    sizeof(value),
                                                    &value_len) == PROTO_ERR_MALFORMED);
        invalid = identity;
        invalid.original_msg_type = MSG_GATEWAY_HOST_RECEIPT;
        assert(gateway_host_receipt_identity_encode(&invalid,
                                                    value,
                                                    sizeof(value),
                                                    &value_len) == PROTO_ERR_MALFORMED);
        invalid = identity;
        invalid.session_id = 0u;
        assert(gateway_host_receipt_identity_encode(&invalid,
                                                    value,
                                                    sizeof(value),
                                                    &value_len) == PROTO_ERR_MALFORMED);
    }

    packet.flags = FLAG_DIAGNOSTIC;
    assert(gateway_host_receipt_packet_validate(&packet,
                                                payload,
                                                payload_len,
                                                &decoded) == PROTO_ERR_MALFORMED);
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

static void test_operation_policy_tlv_registration(void)
{
    const uint8_t policy[] = {1u, 3u, 0u, 2u, 4u};
    uint8_t payload[16] = {0};
    const uint8_t *decoded = NULL;
    uint8_t decoded_len = 0u;
    size_t payload_len = 0u;

    assert(TLV_OPERATION_POLICY == 0xAEu);
    assert(tlv_append_bytes(payload, sizeof(payload), &payload_len,
                            TLV_OPERATION_POLICY,
                            policy, sizeof(policy)) == PROTO_OK);
    assert(tlv_find(payload, payload_len, TLV_OPERATION_POLICY,
                    &decoded, &decoded_len) == PROTO_OK);
    assert(decoded_len == sizeof(policy));
    assert(memcmp(decoded, policy, sizeof(policy)) == 0);
}

int main(void)
{
    test_crc_known_vector();
    test_click_report_transport_session_identity();
    test_tlv_and_packet_round_trip();
    test_decode_rejects_bad_crc();
    test_extended_packet_round_trip();
    test_packet_rejects_retired_and_compact_only_message_types();
    test_mesh_event_packet_types_round_trip();
    test_collection_control_packet_types_round_trip();
    test_command_result_id_tlvs_round_trip_and_require_identity();
    test_result_offer_grant_busy_tlvs_round_trip();
    test_result_offer_requires_full_semantic_commitment();
    test_result_bundle_and_collection_eack_tlvs_round_trip();
    test_collection_eack_rejects_ambiguous_lists_and_schema_smuggling();
    test_unique_tlv_and_result_decoders_reject_duplicates();
    test_report_validators_reject_schema_smuggling_and_mismatch();
    test_result_busy_disambiguates_alternate_from_correlation();
    test_gateway_host_receipt_identity_round_trip_and_strictness();
    test_cobs_round_trip();
    test_operation_policy_tlv_registration();
    return 0;
}
