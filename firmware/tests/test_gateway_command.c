#include "gateway_command.h"

#include "gateway_membership.h"
#include "mesh.h"

#include <assert.h>
#include <string.h>

#define GATEWAY_ID_TEST 0x9999888877776666ull
#define ANCHOR_ID_TEST 0x1111222233334444ull

static void make_command_payload(uint8_t *payload,
                                 size_t payload_cap,
                                 size_t *payload_len,
                                 enum command_id command_id)
{
    *payload_len = 0u;
    assert(mesh_append_command_id(payload, payload_cap, payload_len, command_id) == PROTO_OK);
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

static void make_collection_result_payload(uint8_t *payload,
                                           size_t payload_cap,
                                           size_t *payload_len,
                                           const struct command_result_id *id,
                                           uint32_t collection_epoch_id)
{
    *payload_len = 0u;
    assert(gateway_command_append_collection_result_identity(payload,
                                                            payload_cap,
                                                            payload_len,
                                                            id,
                                                            collection_epoch_id) == PROTO_OK);
    assert(mesh_append_command_result(payload,
                                      payload_cap,
                                      payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
}

static struct proto_packet make_collection_result_packet(const struct command_result_id *id,
                                                        size_t payload_len)
{
    struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = id->node_id,
        .dst_id = id->gateway_id,
        .session_id = id->command_seq,
        .seq = id->result_seq,
        .ttl = 1u,
        .payload_len = (uint16_t)payload_len,
    };

    return result;
}

static void append_collection_result_record(uint8_t *payload,
                                            size_t payload_cap,
                                            size_t *payload_len,
                                            const struct command_result_id *id,
                                            const uint8_t *result_payload,
                                            size_t result_payload_len)
{
    const struct result_bundle_record record = {
        .result_id = *id,
        .payload_len = (uint16_t)result_payload_len,
        .payload_crc = proto_crc16_ccitt_false(result_payload, result_payload_len),
        .payload = result_payload,
    };

    assert(result_payload_len <= RESULT_BUNDLE_RECORD_MAX_PAYLOAD_LEN);
    assert(result_bundle_record_append_tlv(payload,
                                           payload_cap,
                                           payload_len,
                                           &record) == PROTO_OK);
}

static void test_prepare_outbound_normalizes_host_command(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet host = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC | FLAG_GATEWAY_ACK_REQUIRED | FLAG_COUNT_AS_CLICK,
        .src_id = 0xAABBCCDDu,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 0u,
        .seq = 0u,
        .ttl = 0u,
    };
    struct mesh_outbound out = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    host.payload_len = (uint8_t)payload_len;

    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            1234u,
                                            9u,
                                            &out,
                                            &command_id) == PROTO_OK);
    assert(command_id == CMD_GET_STATUS);
    assert(out.packet.msg_type == MSG_COMMAND);
    assert(out.packet.src_id == GATEWAY_ID_TEST);
    assert(out.packet.dst_id == ANCHOR_ID_TEST);
    assert(out.packet.session_id == 1234u);
    assert(out.packet.seq == 9u);
    assert(out.packet.ttl == MESH_DEFAULT_TTL);
    assert(out.packet.flags == FLAG_DIAGNOSTIC);
    assert(gateway_command_transport_mode_from_outbound(&out) ==
           GATEWAY_COMMAND_TRANSPORT_UNICAST_TRACKED);
    assert(out.payload_len == payload_len);
    assert(memcmp(out.payload, payload, payload_len) == 0);
}

static void test_prepare_outbound_preserves_host_session_sequence_and_ttl(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet host = {
        .msg_type = MSG_COMMAND,
        .src_id = 0u,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 77u,
        .seq = 22u,
        .ttl = 2u,
    };
    struct mesh_outbound out = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    host.payload_len = (uint8_t)payload_len;

    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            555u,
                                            10u,
                                            &out,
                                            &command_id) == PROTO_OK);
    assert(command_id == CMD_PING);
    assert(out.packet.session_id == 77u);
    assert(out.packet.seq == 22u);
    assert(out.packet.ttl == 2u);
    assert(out.packet.flags == 0u);
}

static void test_prepare_outbound_rejects_invalid_host_packets(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet host = {
        .msg_type = MSG_COMMAND,
        .dst_id = ANCHOR_ID_TEST,
    };
    struct mesh_outbound out = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    host.payload_len = (uint8_t)payload_len;

    host.msg_type = MSG_CLICK_REPORT;
    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            1u,
                                            1u,
                                            &out,
                                            &command_id) == PROTO_ERR_ARG);

    host.msg_type = MSG_COMMAND;
    host.dst_id = GATEWAY_ID_TEST;
    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            1u,
                                            1u,
                                            &out,
                                            &command_id) == PROTO_ERR_ARG);

    host.dst_id = MESH_BROADCAST_ID;
    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            1u,
                                            1u,
                                            &out,
                                            &command_id) == PROTO_ERR_ARG);
}

static void test_prepare_outbound_rejects_malformed_command_id(void)
{
    const uint8_t payload[] = {TLV_COMMAND_ID, 1u, 0x01u};
    struct proto_packet host = {
        .msg_type = MSG_COMMAND,
        .dst_id = ANCHOR_ID_TEST,
        .payload_len = sizeof(payload),
    };
    struct mesh_outbound out = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            sizeof(payload),
                                            GATEWAY_ID_TEST,
                                            1u,
                                            1u,
                                            &out,
                                            &command_id) == PROTO_ERR_MALFORMED);
}

static void test_extract_options_defaults_to_single_node_small_result(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_SINGLE_NODE);
    assert(options.response_mode == CMD_RESPONSE_SMALL_RESULT);
    assert(options.command_expiry_s == COMMAND_RESULT_EXPIRY_DEFAULT_S);
    assert(!options.flood_required);
    assert(!options.collection_required);
}

static void test_prepare_outbound_accepts_all_registered_command_flood_with_roster(void)
{
    uint8_t payload[128];
    size_t payload_len = 0u;
    struct proto_packet host = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC | FLAG_GATEWAY_ACK_REQUIRED,
        .src_id = 0xAABBCCDDu,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 0u,
        .seq = 0u,
        .ttl = 0u,
    };
    struct mesh_outbound out = {0};
    struct gateway_command_options options = {0};
    enum command_id command_id = CMD_VENDOR_BASE;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          2u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_ID,
                          ANCHOR_ID_TEST) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_ID,
                          0x5555666677778888ull) == PROTO_OK);
    host.payload_len = (uint8_t)payload_len;

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_ALL_REGISTERED);
    assert(options.response_mode == CMD_RESPONSE_SMALL_RESULT);
    assert(options.command_seq == 1001u);
    assert(options.flood_epoch_id == 2002u);
    assert(options.membership_epoch == 3u);
    assert(options.expected_node_count == 2u);
    assert(options.expected_node_id_count == 2u);
    assert(options.collection_epoch_id == 3003u);
    assert(options.collection_slot_seed == 4004u);
    assert(options.flood_required);
    assert(options.collection_required);

    assert(gateway_command_prepare_outbound(&host,
                                            payload,
                                            payload_len,
                                            GATEWAY_ID_TEST,
                                            1234u,
                                            9u,
                                            &out,
                                            &command_id) == PROTO_OK);
    assert(command_id == CMD_GET_STATUS);
    assert(out.packet.src_id == GATEWAY_ID_TEST);
    assert(out.packet.dst_id == MESH_BROADCAST_ID);
    assert(out.packet.session_id == 1001u);
    assert(out.packet.seq == 9u);
    assert(out.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(out.packet.flags == FLAG_DIAGNOSTIC);
    assert(out.next_hop_id == MESH_BROADCAST_ID);
    assert(out.radio_channel == UWB_CHANNEL_WAKE_CONTACT);
    assert(gateway_command_transport_mode_from_outbound(&out) ==
           GATEWAY_COMMAND_TRANSPORT_C5_BROADCAST);
    assert(out.payload_len == payload_len);
    assert(memcmp(out.payload, payload, payload_len) == 0);
}

static void test_extract_options_accepts_all_registered_collection_without_roster(void)
{
    uint8_t payload[96];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          2u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_ALL_REGISTERED);
    assert(options.membership_epoch == 3u);
    assert(options.expected_node_count == 2u);
    assert(options.expected_node_id_count == 0u);
    assert(options.collection_required);
}

static void test_extract_options_rejects_all_registered_missing_membership_identity(void)
{
    uint8_t payload[96];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_ERR_MALFORMED);
}

static void test_command_flood_requires_collection_identity_for_responses(void)
{
    uint8_t payload[64];
    size_t payload_len = 0u;
    const uint8_t *response_mode_value = NULL;
    uint8_t response_mode_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          12u) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_ERR_MALFORMED);

    assert(tlv_find(payload,
                    payload_len,
                    TLV_COMMAND_RESPONSE_MODE,
                    &response_mode_value,
                    &response_mode_len) == PROTO_OK);
    assert(response_mode_len == 1u);
    payload[(size_t)(response_mode_value - payload)] = CMD_RESPONSE_NONE;
    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_ALL_REGISTERED);
    assert(options.response_mode == CMD_RESPONSE_NONE);
    assert(options.flood_required);
    assert(!options.collection_required);
}

static void test_extract_options_accepts_all_registered_roster(void)
{
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct gateway_command_options options;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          2u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_ID,
                          0x1111222233334444ull) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_ID,
                          0x5555666677778888ull) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.expected_node_count == 2u);
    assert(options.expected_node_id_count == 2u);
    assert(options.expected_node_ids[0] == 0x1111222233334444ull);
    assert(options.expected_node_ids[1] == 0x5555666677778888ull);
}

static void test_extract_options_rejects_mismatched_roster(void)
{
    uint8_t payload[PACKET_MAX_PAYLOAD_LEN];
    size_t payload_len = 0u;
    struct gateway_command_options options;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          2u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);
    assert(tlv_append_u64(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_ID,
                          0x1111222233334444ull) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_ERR_MALFORMED);
}

static void test_tracking_mode_keeps_single_node_on_legacy_wait(void)
{
    uint8_t payload[24];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_SINGLE_NODE);
    assert(options.response_mode == CMD_RESPONSE_NONE);
    assert(!options.collection_required);
    assert(gateway_command_tracking_mode_from_options(&options) ==
           GATEWAY_COMMAND_TRACK_LEGACY_RESULT);
}

static void test_tracking_mode_skips_broadcast_no_response_wait(void)
{
    uint8_t payload[64];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_SET_LED_PATTERN);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_REGISTERED) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_NONE) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          12u) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_ALL_REGISTERED);
    assert(options.response_mode == CMD_RESPONSE_NONE);
    assert(options.flood_required);
    assert(!options.collection_required);
    assert(gateway_command_tracking_mode_from_options(&options) ==
           GATEWAY_COMMAND_TRACK_NONE);
}

static void test_tracking_mode_uses_collection_for_broadcast_results(void)
{
    uint8_t payload[96];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_RESPONSE_MODE,
                         CMD_RESPONSE_SMALL_RESULT) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_SEQ,
                          1001u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_FLOOD_EPOCH_ID,
                          2002u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_MEMBERSHIP_EPOCH,
                          3u) == PROTO_OK);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_EXPECTED_NODE_COUNT,
                          12u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_EPOCH_ID,
                          3003u) == PROTO_OK);
    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COLLECTION_SLOT_SEED,
                          4004u) == PROTO_OK);

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.collection_required);
    assert(gateway_command_tracking_mode_from_options(&options) ==
           GATEWAY_COMMAND_TRACK_COLLECTION);
}

static void test_command_receive_timing_waits_until_execute_delay(void)
{
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_NONE,
        .command_seq = 77u,
        .flood_epoch_id = 88u,
        .execute_delay_ms = 1000u,
        .command_expiry_s = 3u,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
        .message_age_ms = 250u,
    };

    assert(!gateway_command_receive_expired(&packet, &options));
    assert(gateway_command_execute_delay_remaining_ms(&packet, &options) == 750u);

    packet.message_age_ms = 1000u;
    assert(!gateway_command_receive_expired(&packet, &options));
    assert(gateway_command_execute_delay_remaining_ms(&packet, &options) == 0u);
}

static void test_command_receive_rejects_expired_or_unexecutable_delay(void)
{
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_SMALL_RESULT,
        .command_seq = 77u,
        .flood_epoch_id = 88u,
        .execute_delay_ms = 1000u,
        .command_expiry_s = 1u,
        .flood_required = true,
        .collection_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
        .message_age_ms = 999u,
    };

    assert(gateway_command_receive_expired(&packet, &options));

    options.execute_delay_ms = 0u;
    packet.message_age_ms = 1000u;
    assert(gateway_command_receive_expired(&packet, &options));
    assert(gateway_command_expiry_remaining_ms(&packet, &options) == 0u);

    packet.message_age_ms = 999u;
    assert(!gateway_command_receive_expired(&packet, &options));
    assert(gateway_command_expiry_remaining_ms(&packet, &options) == 1u);
}

static void test_command_rx_duplicate_cache_blocks_replay_until_expiry(void)
{
    struct gateway_command_rx_duplicate_cache cache = {0};
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_NONE,
        .command_seq = 77u,
        .flood_epoch_id = 88u,
        .command_expiry_s = 2u,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
        .message_age_ms = 100u,
    };

    assert(!gateway_command_rx_duplicate_seen(&cache, options.command_seq, 1000u));
    gateway_command_rx_duplicate_store(&cache, &packet, &options, 1000u);
    assert(gateway_command_rx_duplicate_seen(&cache, options.command_seq, 1000u));
    assert(gateway_command_rx_duplicate_seen(&cache, options.command_seq, 2899u));
    assert(!gateway_command_rx_duplicate_seen(&cache, options.command_seq, 2900u));
}

static void test_collection_initial_due_is_deterministic_and_bounded(void)
{
    uint32_t due_a;
    uint32_t due_b;
    uint32_t due_other;

    assert(gateway_command_collection_spread_ms(0u) == COLLECTION_INITIAL_SPREAD_MIN_MS);
    assert(gateway_command_collection_spread_ms(1u) == COLLECTION_INITIAL_SPREAD_MIN_MS);
    assert(gateway_command_collection_spread_ms(200u) ==
           200u * COLLECTION_INITIAL_SPREAD_PER_NODE_MS);

    due_a = gateway_command_collection_initial_due_ms(10000u,
                                                      ANCHOR_ID_TEST,
                                                      1001u,
                                                      4004u,
                                                      200u);
    due_b = gateway_command_collection_initial_due_ms(10000u,
                                                      ANCHOR_ID_TEST,
                                                      1001u,
                                                      4004u,
                                                      200u);
    due_other = gateway_command_collection_initial_due_ms(10000u,
                                                          ANCHOR_ID_TEST + 1u,
                                                          1001u,
                                                          4004u,
                                                          200u);

    assert(due_a == due_b);
    assert(due_a >= 10000u);
    assert(due_a < 10000u + gateway_command_collection_spread_ms(200u));
    assert(due_other >= 10000u);
    assert(due_other < 10000u + gateway_command_collection_spread_ms(200u));
    assert(due_other != due_a);
}

static void test_all_node_collection_due_spreads_responses(void)
{
    enum { NODE_COUNT = 64u };
    const uint32_t command_flood_end_ms = 120000u;
    const uint32_t command_seq = 0x10203040u;
    const uint32_t slot_seed = 0xa5a55a5au;
    const uint16_t expected_node_count = 128u;
    const uint32_t spread_ms = gateway_command_collection_spread_ms(expected_node_count);
    uint32_t due_times[NODE_COUNT];
    uint32_t min_due = command_flood_end_ms + spread_ms;
    uint32_t max_due = command_flood_end_ms;
    uint8_t immediate_count = 0u;
    uint8_t first_second_count = 0u;
    uint8_t unique_count = 0u;

    assert(spread_ms == expected_node_count * COLLECTION_INITIAL_SPREAD_PER_NODE_MS);

    for (uint8_t i = 0u; i < NODE_COUNT; i++) {
        uint64_t node_id = ANCHOR_ID_TEST + (uint64_t)i;
        uint32_t due = gateway_command_collection_initial_due_ms(command_flood_end_ms,
                                                                 node_id,
                                                                 command_seq,
                                                                 slot_seed,
                                                                 expected_node_count);
        bool unique = true;

        assert(due == gateway_command_collection_initial_due_ms(command_flood_end_ms,
                                                                node_id,
                                                                command_seq,
                                                                slot_seed,
                                                                expected_node_count));
        assert(due >= command_flood_end_ms);
        assert(due < command_flood_end_ms + spread_ms);

        if (due == command_flood_end_ms) {
            immediate_count++;
        }
        if (due < command_flood_end_ms + 1000u) {
            first_second_count++;
        }
        if (due < min_due) {
            min_due = due;
        }
        if (due > max_due) {
            max_due = due;
        }
        for (uint8_t j = 0u; j < i; j++) {
            if (due_times[j] == due) {
                unique = false;
                break;
            }
        }
        due_times[i] = due;
        if (unique) {
            unique_count++;
        }
    }

    assert(immediate_count <= 1u);
    assert(first_second_count <= 8u);
    assert(unique_count >= 48u);
    assert(max_due - min_due > (spread_ms / 2u));
}

static void test_collection_retry_round_advances_spread(void)
{
    struct gateway_collection_state collection;

    assert(gateway_command_collection_retry_spread_ms(0u) == COLLECTION_RETRY_ROUND_0_MS);
    assert(gateway_command_collection_retry_spread_ms(1u) == COLLECTION_RETRY_ROUND_1_MS);
    assert(gateway_command_collection_retry_spread_ms(2u) == COLLECTION_RETRY_ROUND_2_MS);
    assert(gateway_command_collection_retry_spread_ms(3u) == COLLECTION_RETRY_ROUND_3_MS);
    assert(gateway_command_collection_retry_spread_ms(4u) == COLLECTION_RETRY_ROUND_STEADY_MS);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    12u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_advance_retry_round(&collection) == PROTO_OK);
    assert(collection.retry_round == 1u);
    assert(collection.next_retry_spread_ms == COLLECTION_RETRY_ROUND_1_MS);
    assert(gateway_collection_advance_retry_round(&collection) == PROTO_OK);
    assert(collection.retry_round == 2u);
    assert(collection.next_retry_spread_ms == COLLECTION_RETRY_ROUND_2_MS);
}

static void test_append_collection_result_identity_requires_epoch(void)
{
    const struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id decoded = {0};
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    uint8_t payload[96];
    size_t payload_len = 0u;

    assert(gateway_command_append_collection_result_identity(payload,
                                                            sizeof(payload),
                                                            &payload_len,
                                                            &id,
                                                            3003u) == PROTO_OK);
    assert(command_result_id_from_tlvs(payload, payload_len, &decoded) == PROTO_OK);
    assert_command_result_id_equal(&decoded, &id);
    assert(tlv_find(payload,
                    payload_len,
                    TLV_COLLECTION_EPOCH_ID,
                    &value,
                    &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    assert(proto_get_u32_le(value) == 3003u);

    payload_len = 0u;
    assert(gateway_command_append_collection_result_identity(payload,
                                                            sizeof(payload),
                                                            &payload_len,
                                                            &id,
                                                            0u) == PROTO_ERR_MALFORMED);
}

static void test_collection_records_unique_results_and_builds_eack(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_eack eack = {0};
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet result_a;
    struct proto_packet result_b;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    bool duplicate = true;

    id_b.node_id = 0x2222333344445555ull;
    id_b.result_seq = 2u;
    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    1u,
                                    COLLECTION_RETRY_ROUND_1_MS) == PROTO_OK);
    assert(collection.collection_open);

    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 1u);
    assert(collection.collection_open);
    assert(gateway_collection_contains_result(&collection, &id_a));
    assert_command_result_id_equal(&collection.results[0].id, &id_a);
    assert(collection.results[0].payload_crc ==
           proto_crc16_ccitt_false(payload_a, payload_a_len));
    assert(collection.results[0].payload_len == payload_a_len);

    duplicate = false;
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(collection.received_count == 1u);

    assert(gateway_collection_record_result(&collection,
                                            &result_b,
                                            payload_b,
                                            payload_b_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 2u);
    assert(!collection.collection_open);

    assert(gateway_collection_build_eack(&collection,
                                         EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                                         &eack) == PROTO_OK);
    assert(eack.gateway_id == GATEWAY_ID_TEST);
    assert(eack.gateway_epoch == 9u);
    assert(eack.command_seq == 1001u);
    assert(eack.collection_epoch_id == 3003u);
    assert(eack.membership_epoch == 4u);
    assert(eack.expected_count == 2u);
    assert(eack.received_count == 2u);
    assert(eack.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST);
    assert(eack.retry_round == 1u);
    assert(eack.next_retry_spread_ms == COLLECTION_RETRY_ROUND_1_MS);
    assert(!eack.collection_open);

    duplicate = false;
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(collection.received_count == 2u);
}

static void test_collection_prepares_eack_broadcast_outbound(void)
{
    struct gateway_collection_state collection;
    struct mesh_outbound out = {0};
    struct gateway_collection_eack decoded = {0};
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = 0xAAA1u,
        .node_boot_counter = 0x01020304u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = 0xBBB2u,
        .node_boot_counter = 0x05060708u,
        .result_seq = 1u,
    };
    struct proto_packet result_a;
    struct proto_packet result_b;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    bool duplicate = false;
    bool listed = false;

    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    12u,
                                    2u,
                                    COLLECTION_RETRY_ROUND_2_MS) == PROTO_OK);
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result(&collection,
                                            &result_b,
                                            payload_b,
                                            payload_b_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(gateway_collection_prepare_eack_outbound(&collection,
                                                    EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                                                    &out) == PROTO_OK);
    assert(out.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK);
    assert(out.packet.src_id == GATEWAY_ID_TEST);
    assert(out.packet.dst_id == MESH_BROADCAST_ID);
    assert(out.packet.session_id == 1001u);
    assert(out.packet.seq == 2u);
    assert(out.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(out.packet.payload_len == out.payload_len);
    assert(out.next_hop_id == MESH_BROADCAST_ID);
    assert(out.radio_channel == UWB_CHANNEL_WAKE_CONTACT);

    assert(gateway_collection_eack_from_tlvs(out.payload,
                                             out.payload_len,
                                             &decoded) == PROTO_OK);
    assert(decoded.gateway_id == GATEWAY_ID_TEST);
    assert(decoded.gateway_epoch == 9u);
    assert(decoded.command_seq == 1001u);
    assert(decoded.collection_epoch_id == 3003u);
    assert(decoded.membership_epoch == 4u);
    assert(decoded.expected_count == 12u);
    assert(decoded.received_count == 2u);
    assert(decoded.eack_format == EACK_FORMAT_EXPLICIT_RECEIVED_LIST);
    assert(decoded.retry_round == 2u);
    assert(decoded.next_retry_spread_ms == COLLECTION_RETRY_ROUND_2_MS);
    assert(decoded.collection_open);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    id_a.node_id,
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    id_b.node_id,
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    0xCCC3u,
                                                    &listed) == PROTO_OK);
    assert(!listed);
}

static void test_collection_prepares_missing_list_from_roster(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    const uint64_t roster[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
        0x4444555566667777ull,
        0x5555666677778888ull,
    };
    struct proto_packet result_a;
    struct proto_packet result_b;
    struct mesh_outbound out;
    struct gateway_collection_eack decoded;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    uint16_t missing_count = 0u;
    bool duplicate = false;
    bool listed = true;

    id_b.node_id = roster[1];
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    4u,
                                    2u,
                                    COLLECTION_RETRY_ROUND_2_MS) == PROTO_OK);
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result(&collection,
                                            &result_b,
                                            payload_b,
                                            payload_b_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(gateway_collection_prepare_missing_eack_outbound(&collection,
                                                            roster,
                                                            sizeof(roster) / sizeof(roster[0]),
                                                            &out,
                                                            &missing_count) == PROTO_OK);
    assert(missing_count == 2u);
    assert(out.packet.msg_type == MSG_GATEWAY_COLLECTION_EACK);
    assert(out.packet.dst_id == MESH_BROADCAST_ID);
    assert(out.radio_channel == UWB_CHANNEL_WAKE_CONTACT);

    assert(gateway_collection_eack_from_tlvs(out.payload,
                                             out.payload_len,
                                             &decoded) == PROTO_OK);
    assert(decoded.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST);
    assert(decoded.expected_count == 4u);
    assert(decoded.received_count == 2u);
    assert(decoded.collection_open);

    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    roster[0],
                                                    &listed) == PROTO_OK);
    assert(!listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    roster[1],
                                                    &listed) == PROTO_OK);
    assert(!listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    roster[2],
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    roster[3],
                                                    &listed) == PROTO_OK);
    assert(listed);
}

static void test_collection_roster_resolution_prefers_explicit_roster(void)
{
    const uint64_t provider_ids[] = {
        0x9999000000000001ull,
    };
    struct gateway_membership_roster membership;
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_SMALL_RESULT,
        .membership_epoch = 7u,
        .expected_node_count = 2u,
        .expected_node_id_count = 2u,
        .collection_required = true,
        .expected_node_ids = {
            ANCHOR_ID_TEST,
            0x5555666677778888ull,
        },
    };
    uint64_t resolved[2] = {0};
    size_t resolved_count = 0u;
    enum gateway_command_collection_roster_source source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;

    gateway_membership_clear(&membership);
    assert(gateway_membership_set_roster_preserve_order(&membership,
                                                        99u,
                                                        provider_ids,
                                                        1u) == PROTO_OK);

    assert(gateway_command_resolve_collection_roster(&options,
                                                     &membership,
                                                     resolved,
                                                     sizeof(resolved) / sizeof(resolved[0]),
                                                     &resolved_count,
                                                     &source) == PROTO_OK);
    assert(source == GATEWAY_COMMAND_COLLECTION_ROSTER_EXPLICIT);
    assert(resolved_count == 2u);
    assert(resolved[0] == ANCHOR_ID_TEST);
    assert(resolved[1] == 0x5555666677778888ull);
}

static void test_collection_roster_resolution_rejects_provider_mismatch(void)
{
    const uint64_t roster[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
    };
    struct gateway_membership_roster membership;
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_SMALL_RESULT,
        .membership_epoch = 7u,
        .expected_node_count = 2u,
        .collection_required = true,
    };
    uint64_t resolved[3] = {0};
    size_t resolved_count = 0u;
    enum gateway_command_collection_roster_source source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;

    gateway_membership_clear(&membership);
    assert(gateway_membership_set_roster_preserve_order(&membership,
                                                        8u,
                                                        roster,
                                                        2u) == PROTO_OK);
    assert(gateway_command_resolve_collection_roster(&options,
                                                     &membership,
                                                     resolved,
                                                     sizeof(resolved) / sizeof(resolved[0]),
                                                     &resolved_count,
                                                     &source) == PROTO_ERR_STALE);

    assert(gateway_membership_set_roster_preserve_order(&membership,
                                                        7u,
                                                        roster,
                                                        2u) == PROTO_OK);
    options.expected_node_count = 3u;
    assert(gateway_command_resolve_collection_roster(&options,
                                                     &membership,
                                                     resolved,
                                                     sizeof(resolved) / sizeof(resolved[0]),
                                                     &resolved_count,
                                                     &source) == PROTO_ERR_MALFORMED);
}

static void test_collection_roster_resolution_leaves_all_heard_best_effort(void)
{
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_HEARD,
        .response_mode = CMD_RESPONSE_SMALL_RESULT,
        .membership_epoch = 7u,
        .expected_node_count = 2u,
        .collection_required = true,
    };
    uint64_t resolved[2] = {0};
    size_t resolved_count = 99u;
    enum gateway_command_collection_roster_source source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_EXPLICIT;

    assert(gateway_command_resolve_collection_roster(&options,
                                                     NULL,
                                                     resolved,
                                                     sizeof(resolved) / sizeof(resolved[0]),
                                                     &resolved_count,
                                                     &source) == PROTO_OK);
    assert(source == GATEWAY_COMMAND_COLLECTION_ROSTER_NONE);
    assert(resolved_count == 0u);
}

static void test_provider_roster_feeds_missing_list_eack(void)
{
    const uint64_t provider_ids[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
        0x4444555566667777ull,
    };
    struct gateway_membership_roster membership;
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .response_mode = CMD_RESPONSE_SMALL_RESULT,
        .membership_epoch = 7u,
        .expected_node_count = 3u,
        .collection_required = true,
    };
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result_a;
    struct mesh_outbound out;
    struct gateway_collection_eack decoded;
    uint8_t payload_a[96];
    size_t payload_a_len = 0u;
    uint64_t resolved[3] = {0};
    size_t resolved_count = 0u;
    uint16_t missing_count = 0u;
    bool duplicate = false;
    bool listed = false;
    enum gateway_command_collection_roster_source source =
        GATEWAY_COMMAND_COLLECTION_ROSTER_NONE;

    gateway_membership_clear(&membership);
    assert(gateway_membership_set_roster_preserve_order(&membership,
                                                        7u,
                                                        provider_ids,
                                                        sizeof(provider_ids) / sizeof(provider_ids[0])) ==
           PROTO_OK);

    assert(gateway_command_resolve_collection_roster(&options,
                                                     &membership,
                                                     resolved,
                                                     sizeof(resolved) / sizeof(resolved[0]),
                                                     &resolved_count,
                                                     &source) == PROTO_OK);
    assert(source == GATEWAY_COMMAND_COLLECTION_ROSTER_MEMBERSHIP);
    assert(resolved_count == 3u);

    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    options.membership_epoch,
                                    options.expected_node_count,
                                    1u,
                                    COLLECTION_RETRY_ROUND_1_MS) == PROTO_OK);
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(gateway_collection_prepare_missing_eack_outbound(&collection,
                                                            resolved,
                                                            resolved_count,
                                                            &out,
                                                            &missing_count) == PROTO_OK);
    assert(missing_count == 2u);
    assert(gateway_collection_eack_from_tlvs(out.payload,
                                             out.payload_len,
                                             &decoded) == PROTO_OK);
    assert(decoded.eack_format == EACK_FORMAT_EXPLICIT_MISSING_LIST);
    assert(decoded.membership_epoch == options.membership_epoch);
    assert(decoded.expected_count == options.expected_node_count);

    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    provider_ids[0],
                                                    &listed) == PROTO_OK);
    assert(!listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    provider_ids[1],
                                                    &listed) == PROTO_OK);
    assert(listed);
    assert(gateway_collection_eack_contains_node_id(out.payload,
                                                    out.payload_len,
                                                    provider_ids[2],
                                                    &listed) == PROTO_OK);
    assert(listed);
}

static void test_collection_records_result_bundle_and_dedupes_replay(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = 55u,
        .record_count = 2u,
    };
    struct proto_packet bundle_packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = 55u,
        .ttl = 1u,
    };
    uint8_t result_payload_a[96];
    uint8_t result_payload_b[96];
    uint8_t records[192];
    uint8_t bundle_payload[256];
    size_t result_payload_a_len = 0u;
    size_t result_payload_b_len = 0u;
    size_t records_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    id_b.node_id = 0x3333444455556666ull;
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    make_collection_result_payload(result_payload_a,
                                   sizeof(result_payload_a),
                                   &result_payload_a_len,
                                   &id_a,
                                   3003u);
    make_collection_result_payload(result_payload_b,
                                   sizeof(result_payload_b),
                                   &result_payload_b_len,
                                   &id_b,
                                   3003u);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id_a,
                                    result_payload_a,
                                    result_payload_a_len);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id_b,
                                    result_payload_b,
                                    result_payload_b_len);
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(bundle_payload,
                                            sizeof(bundle_payload),
                                            &bundle_payload_len,
                                            &bundle) == PROTO_OK);
    assert(sizeof(bundle_payload) - bundle_payload_len >= records_len);
    memcpy(&bundle_payload[bundle_payload_len], records, records_len);
    bundle_payload_len += records_len;
    bundle_packet.payload_len = (uint16_t)bundle_payload_len;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_OK);
    assert(accepted_count == 2u);
    assert(duplicate_count == 0u);
    assert(collection.received_count == 2u);
    assert(!collection.collection_open);
    assert(gateway_collection_contains_result(&collection, &id_a));
    assert(gateway_collection_contains_result(&collection, &id_b));

    accepted_count = 99u;
    duplicate_count = 99u;
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_OK);
    assert(accepted_count == 0u);
    assert(duplicate_count == 2u);
    assert(collection.received_count == 2u);
}

static void test_collection_return_candidates_from_direct_results(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet result_a;
    struct proto_packet result_b;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    uint64_t candidates[2] = {0};
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    size_t candidate_count = 0u;
    bool duplicate = false;

    id_b.node_id = 0x2222333344445555ull;
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_a,
                                                     payload_a,
                                                     payload_a_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_b,
                                                     payload_b,
                                                     payload_b_len,
                                                     0xAAAABBBBCCCC0002ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(collection.results[0].previous_hop_id == 0xAAAABBBBCCCC0001ull);
    assert(collection.results[1].previous_hop_id == 0xAAAABBBBCCCC0002ull);
    candidate_count = gateway_collection_return_candidates(&collection,
                                                           candidates,
                                                           sizeof(candidates) / sizeof(candidates[0]));
    assert(candidate_count == 2u);
    assert(candidates[0] == 0xAAAABBBBCCCC0002ull);
    assert(candidates[1] == 0xAAAABBBBCCCC0001ull);
}

static void test_collection_snapshot_round_trips_return_hops(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    struct gateway_collection_state_snapshot snapshot;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet result_a;
    struct proto_packet result_b;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    uint64_t candidates[2] = {0};
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    bool duplicate = false;

    id_b.node_id = 0x2222333344445555ull;
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    3u,
                                    1u,
                                    1200u) == PROTO_OK);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_a,
                                                     payload_a,
                                                     payload_a_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_b,
                                                     payload_b,
                                                     payload_b_len,
                                                     0xAAAABBBBCCCC0002ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);

    memset(&snapshot, 0xA5, sizeof(snapshot));
    assert(gateway_collection_export_snapshot(&collection, &snapshot) == PROTO_OK);
    assert(snapshot.version == GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION);
    assert(snapshot.valid);
    assert(snapshot.result_count == 2u);
    assert(snapshot.results[0].previous_hop_id == 0xAAAABBBBCCCC0001ull);
    assert(snapshot.results[1].previous_hop_id == 0xAAAABBBBCCCC0002ull);

    memset(&restored, 0, sizeof(restored));
    assert(gateway_collection_restore_snapshot(&restored, &snapshot) == PROTO_OK);
    assert(restored.gateway_id == collection.gateway_id);
    assert(restored.gateway_epoch == collection.gateway_epoch);
    assert(restored.command_seq == collection.command_seq);
    assert(restored.collection_epoch_id == collection.collection_epoch_id);
    assert(restored.membership_epoch == collection.membership_epoch);
    assert(restored.expected_count == collection.expected_count);
    assert(restored.received_count == collection.received_count);
    assert(restored.retry_round == collection.retry_round);
    assert(restored.next_retry_spread_ms == collection.next_retry_spread_ms);
    assert(restored.collection_open);
    assert_command_result_id_equal(&restored.results[0].id, &id_a);
    assert_command_result_id_equal(&restored.results[1].id, &id_b);
    assert(restored.results[0].previous_hop_id == 0xAAAABBBBCCCC0001ull);
    assert(restored.results[1].previous_hop_id == 0xAAAABBBBCCCC0002ull);
    assert(gateway_collection_return_candidates(&restored,
                                                candidates,
                                                sizeof(candidates) / sizeof(candidates[0])) == 2u);
    assert(candidates[0] == 0xAAAABBBBCCCC0002ull);
    assert(candidates[1] == 0xAAAABBBBCCCC0001ull);
}

static void test_collection_snapshot_round_trips_closed_collection(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_state restored;
    struct gateway_collection_state_snapshot snapshot;
    struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    bool duplicate = false;

    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    1u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result,
                                                     payload,
                                                     payload_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == collection.expected_count);
    assert(!collection.collection_open);

    assert(gateway_collection_export_snapshot(&collection, &snapshot) == PROTO_OK);
    memset(&restored, 0, sizeof(restored));
    assert(gateway_collection_restore_snapshot(&restored, &snapshot) == PROTO_OK);
    assert(restored.received_count == 1u);
    assert(restored.expected_count == 1u);
    assert(!restored.collection_open);
    assert(restored.results[0].previous_hop_id == 0xAAAABBBBCCCC0001ull);
}

static void test_collection_snapshot_rejects_corrupt_without_partial_restore(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_state target;
    struct gateway_collection_state_snapshot snapshot;
    struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    bool duplicate = false;

    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result,
                                                     payload,
                                                     payload_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_export_snapshot(&collection, &snapshot) == PROTO_OK);

    assert(gateway_collection_start(&target,
                                    0x8888777766665555ull,
                                    3u,
                                    42u,
                                    55u,
                                    6u,
                                    1u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    snapshot.version = GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION + 1u;
    assert(gateway_collection_restore_snapshot(&target, &snapshot) == PROTO_ERR_BAD_VERSION);
    assert(target.gateway_id == 0x8888777766665555ull);
    assert(target.command_seq == 42u);
    assert(target.collection_epoch_id == 55u);
    assert(target.received_count == 0u);

    snapshot.version = GATEWAY_COLLECTION_STATE_SNAPSHOT_VERSION;
    snapshot.valid = false;
    assert(gateway_collection_restore_snapshot(&target, &snapshot) == PROTO_ERR_MALFORMED);
    assert(target.gateway_id == 0x8888777766665555ull);
    assert(target.command_seq == 42u);
    assert(target.collection_epoch_id == 55u);
    assert(target.received_count == 0u);

    snapshot.valid = true;
    snapshot.received_count = snapshot.expected_count + 1u;
    assert(gateway_collection_restore_snapshot(&target, &snapshot) == PROTO_ERR_MALFORMED);
    assert(target.gateway_id == 0x8888777766665555ull);
    assert(target.command_seq == 42u);
    assert(target.collection_epoch_id == 55u);
    assert(target.received_count == 0u);

    assert(gateway_collection_export_snapshot(NULL, &snapshot) == PROTO_ERR_ARG);
    assert(gateway_collection_export_snapshot(&collection, NULL) == PROTO_ERR_ARG);
    assert(gateway_collection_restore_snapshot(NULL, &snapshot) == PROTO_ERR_ARG);
    assert(gateway_collection_restore_snapshot(&target, NULL) == PROTO_ERR_ARG);
}

static void test_collection_bundle_records_return_hop(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = 57u,
        .record_count = 2u,
    };
    struct proto_packet bundle_packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = 57u,
        .ttl = 1u,
    };
    uint8_t result_payload_a[96];
    uint8_t result_payload_b[96];
    uint8_t records[192];
    uint8_t bundle_payload[256];
    uint64_t candidates[2] = {0};
    size_t result_payload_a_len = 0u;
    size_t result_payload_b_len = 0u;
    size_t records_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    id_b.node_id = 0x3333444455556666ull;
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    make_collection_result_payload(result_payload_a,
                                   sizeof(result_payload_a),
                                   &result_payload_a_len,
                                   &id_a,
                                   3003u);
    make_collection_result_payload(result_payload_b,
                                   sizeof(result_payload_b),
                                   &result_payload_b_len,
                                   &id_b,
                                   3003u);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id_a,
                                    result_payload_a,
                                    result_payload_a_len);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id_b,
                                    result_payload_b,
                                    result_payload_b_len);
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(bundle_payload,
                                            sizeof(bundle_payload),
                                            &bundle_payload_len,
                                            &bundle) == PROTO_OK);
    memcpy(&bundle_payload[bundle_payload_len], records, records_len);
    bundle_payload_len += records_len;
    bundle_packet.payload_len = (uint16_t)bundle_payload_len;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    assert(gateway_collection_record_bundle_from_hop(&collection,
                                                     &bundle_packet,
                                                     bundle_payload,
                                                     bundle_payload_len,
                                                     bundle_packet.src_id,
                                                     &accepted_count,
                                                     &duplicate_count) == PROTO_OK);
    assert(accepted_count == 2u);
    assert(duplicate_count == 0u);
    assert(collection.results[0].previous_hop_id == bundle_packet.src_id);
    assert(collection.results[1].previous_hop_id == bundle_packet.src_id);
    assert(gateway_collection_return_candidates(&collection,
                                                candidates,
                                                sizeof(candidates) / sizeof(candidates[0])) == 1u);
    assert(candidates[0] == bundle_packet.src_id);
}

static void test_collection_return_candidates_suppress_duplicates(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct command_result_id id_c = id_a;
    struct proto_packet result_a;
    struct proto_packet result_b;
    struct proto_packet result_c;
    uint8_t payload_a[96];
    uint8_t payload_b[96];
    uint8_t payload_c[96];
    uint64_t candidates[3] = {0};
    size_t payload_a_len = 0u;
    size_t payload_b_len = 0u;
    size_t payload_c_len = 0u;
    bool duplicate = false;

    id_b.node_id = 0x2222333344445555ull;
    id_b.node_boot_counter = 78u;
    id_b.result_seq = 2u;
    id_c.node_id = 0x3333444455556666ull;
    id_c.node_boot_counter = 79u;
    id_c.result_seq = 3u;
    make_collection_result_payload(payload_a, sizeof(payload_a), &payload_a_len, &id_a, 3003u);
    make_collection_result_payload(payload_b, sizeof(payload_b), &payload_b_len, &id_b, 3003u);
    make_collection_result_payload(payload_c, sizeof(payload_c), &payload_c_len, &id_c, 3003u);
    result_a = make_collection_result_packet(&id_a, payload_a_len);
    result_b = make_collection_result_packet(&id_b, payload_b_len);
    result_c = make_collection_result_packet(&id_c, payload_c_len);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    4u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_a,
                                                     payload_a,
                                                     payload_a_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_b,
                                                     payload_b,
                                                     payload_b_len,
                                                     MESH_BROADCAST_ID,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(gateway_collection_record_result_from_hop(&collection,
                                                     &result_c,
                                                     payload_c,
                                                     payload_c_len,
                                                     0xAAAABBBBCCCC0001ull,
                                                     &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(gateway_collection_return_candidates(&collection,
                                                candidates,
                                                sizeof(candidates) / sizeof(candidates[0])) == 1u);
    assert(candidates[0] == 0xAAAABBBBCCCC0001ull);
}

static void test_collection_rejects_corrupt_result_bundle(void)
{
    struct gateway_collection_state collection;
    const struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = 56u,
        .record_count = 1u,
    };
    struct proto_packet bundle_packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = 56u,
        .ttl = 1u,
    };
    uint8_t result_payload[96];
    uint8_t records[96];
    uint8_t bundle_payload[160];
    size_t result_payload_len = 0u;
    size_t records_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;

    make_collection_result_payload(result_payload,
                                   sizeof(result_payload),
                                   &result_payload_len,
                                   &id,
                                   3003u);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id,
                                    result_payload,
                                    result_payload_len);
    bundle.bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(bundle_payload,
                                            sizeof(bundle_payload),
                                            &bundle_payload_len,
                                            &bundle) == PROTO_OK);
    memcpy(&bundle_payload[bundle_payload_len], records, records_len);
    bundle_payload_len += records_len;
    bundle_packet.payload_len = (uint16_t)bundle_payload_len;
    bundle_payload[bundle_payload_len - 1u] ^= 0x01u;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    1u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_BAD_CRC);
    assert(accepted_count == 0u);
    assert(duplicate_count == 0u);
    assert(collection.received_count == 0u);
}

static void test_collection_rejects_wrong_result_identity(void)
{
    struct gateway_collection_state collection;
    struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    const uint8_t *command_seq_value = NULL;
    uint8_t command_seq_len = 0u;
    bool duplicate = false;

    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    result.src_id = id.node_id + 1u;
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_MALFORMED);
    result.src_id = id.node_id;

    assert(tlv_find(payload,
                    payload_len,
                    TLV_COMMAND_SEQ,
                    &command_seq_value,
                    &command_seq_len) == PROTO_OK);
    assert(command_seq_len == sizeof(uint32_t));
    payload[(size_t)(command_seq_value - payload)] ^= 0x01u;
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_MALFORMED);
}

static void test_collection_rejects_missing_or_wrong_collection_epoch(void)
{
    struct gateway_collection_state collection;
    const struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result;
    uint8_t payload[96];
    size_t payload_len = 0u;
    bool duplicate = false;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    assert(command_result_id_append_tlvs(payload,
                                         sizeof(payload),
                                         &payload_len,
                                         &id) == PROTO_OK);
    assert(mesh_append_command_result(payload,
                                      sizeof(payload),
                                      &payload_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_NOT_FOUND);

    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3004u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_MALFORMED);
}

static void test_extract_duration_uses_optional_tlv(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    uint32_t duration_ms = 0u;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_START_HEARTBEAT) == PROTO_OK);
    assert(gateway_command_extract_duration_ms(payload,
                                               payload_len,
                                               60000u,
                                               &duration_ms) == PROTO_OK);
    assert(duration_ms == 60000u);

    assert(tlv_append_u32(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_DURATION_MS,
                          15000u) == PROTO_OK);
    assert(gateway_command_extract_duration_ms(payload,
                                               payload_len,
                                               60000u,
                                               &duration_ms) == PROTO_OK);
    assert(duration_ms == 15000u);

    payload[payload_len - 4u] = 0u;
    payload[payload_len - 3u] = 0u;
    payload[payload_len - 2u] = 0u;
    payload[payload_len - 1u] = 0u;
    assert(gateway_command_extract_duration_ms(payload,
                                               payload_len,
                                               60000u,
                                               &duration_ms) == PROTO_ERR_MALFORMED);
}

static void test_extract_role_requires_valid_device_role_tlv(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    enum device_role role = ROLE_CLICKER;

    assert(mesh_append_command_id(payload,
                                  sizeof(payload),
                                  &payload_len,
                                  CMD_SET_ROLE) == PROTO_OK);
    assert(gateway_command_extract_role(payload,
                                        payload_len,
                                        &role) == PROTO_ERR_NOT_FOUND);

    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_DEVICE_ROLE,
                         ROLE_ANCHOR) == PROTO_OK);
    assert(gateway_command_extract_role(payload,
                                        payload_len,
                                        &role) == PROTO_OK);
    assert(role == ROLE_ANCHOR);

    payload[payload_len - 1u] = 99u;
    assert(gateway_command_extract_role(payload,
                                        payload_len,
                                        &role) == PROTO_ERR_MALFORMED);
}

static void test_build_failure_result_is_host_visible(void)
{
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 123u,
        .seq = 44u,
    };
    struct proto_packet result = {0};
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(gateway_command_build_failure_result(&command,
                                                GATEWAY_ID_TEST,
                                                CMD_GET_STATUS,
                                                COMMAND_INVALID_STATE,
                                                7u,
                                                999u,
                                                &result,
                                                payload,
                                                sizeof(payload),
                                                &payload_len) == PROTO_OK);
    assert(result.msg_type == MSG_COMMAND_RESULT);
    assert(result.flags == (FLAG_ERROR | FLAG_DIAGNOSTIC));
    assert(result.src_id == GATEWAY_ID_TEST);
    assert(result.dst_id == GATEWAY_ID_TEST);
    assert(result.session_id == command.session_id);
    assert(result.seq == command.seq);
    assert(result.ttl == 1u);
    assert(result.payload_len == payload_len);

    assert(tlv_find(payload, payload_len, TLV_COMMAND_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == CMD_GET_STATUS);
    assert(tlv_find(payload, payload_len, TLV_COMMAND_STATUS, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == COMMAND_INVALID_STATE);
    assert(tlv_find(payload, payload_len, TLV_REASON, &value, &value_len) == PROTO_OK);
    assert(value_len == 1u);
    assert(value[0] == 7u);
}

static void test_pending_command_completes_on_matching_result(void)
{
    struct gateway_command_pending pending = {0};
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 123u,
        .seq = 44u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet wrong_result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 123u,
        .seq = 44u,
    };
    struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 123u,
        .seq = 44u,
    };

    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_GET_STATUS,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(pending.active);
    assert(pending.deadline_ms == 1000u + GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    assert(!gateway_command_pending_matches_result(&pending, &wrong_result));
    assert(!gateway_command_pending_complete_result(&pending, &wrong_result));
    assert(pending.active);

    assert(gateway_command_pending_matches_result(&pending, &result));
    assert(gateway_command_pending_complete_result(&pending, &result));
    assert(!pending.active);
}

static void test_pending_command_expires_with_original_context(void)
{
    struct gateway_command_pending pending = {0};
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 124u,
        .seq = 45u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet expired_command = {0};
    enum command_id expired_id = CMD_VENDOR_BASE;

    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_PING,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(!gateway_command_pending_expired(&pending,
                                            1000u + GATEWAY_COMMAND_RESULT_TIMEOUT_MS - 1u,
                                            &expired_command,
                                            &expired_id));
    assert(pending.active);

    assert(gateway_command_pending_expired(&pending,
                                           1000u + GATEWAY_COMMAND_RESULT_TIMEOUT_MS,
                                           &expired_command,
                                           &expired_id));
    assert(!pending.active);
    assert(expired_command.msg_type == MSG_COMMAND);
    assert(expired_command.dst_id == ANCHOR_ID_TEST);
    assert(expired_command.session_id == 124u);
    assert(expired_command.seq == 45u);
    assert(expired_id == CMD_PING);
}

static void test_pending_command_expiry_handles_ms_wrap(void)
{
    struct gateway_command_pending pending = {0};
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 126u,
        .seq = 46u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet expired_command = {0};
    enum command_id expired_id = CMD_VENDOR_BASE;
    const uint32_t start_ms = 0xfffffff0u;
    const uint32_t before_deadline_ms = start_ms + 100u;
    const uint32_t deadline_ms = start_ms + GATEWAY_COMMAND_RESULT_TIMEOUT_MS;

    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_PING,
                                         start_ms,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(pending.active);
    assert(pending.deadline_ms < start_ms);
    assert(!gateway_command_pending_expired(&pending,
                                            before_deadline_ms,
                                            &expired_command,
                                            &expired_id));
    assert(pending.active);

    assert(gateway_command_pending_expired(&pending,
                                           deadline_ms,
                                           &expired_command,
                                           &expired_id));
    assert(!pending.active);
    assert(expired_command.session_id == command.session_id);
    assert(expired_command.seq == command.seq);
    assert(expired_id == CMD_PING);
}

static void test_pending_survey_prepare_completes_on_command_result(void)
{
    struct gateway_command_pending pending = {0};
    struct proto_packet prepare = {
        .msg_type = MSG_SURVEY_PAIR_PREPARE,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 125u,
        .seq = 46u,
        .ttl = MESH_DEFAULT_TTL,
    };
    struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 125u,
        .seq = 46u,
    };

    assert(gateway_command_pending_start(&pending,
                                         &prepare,
                                         CMD_SURVEY_PREPARE_PAIR,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(gateway_command_pending_matches_result(&pending, &result));
    assert(gateway_command_pending_complete_result(&pending, &result));
    assert(!pending.active);
}

static void test_pending_command_rejects_second_start(void)
{
    struct gateway_command_pending pending = {0};
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 125u,
        .seq = 46u,
        .ttl = MESH_DEFAULT_TTL,
    };

    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_PING,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_PING,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_prepare_outbound_normalizes_host_command();
    test_prepare_outbound_preserves_host_session_sequence_and_ttl();
    test_prepare_outbound_rejects_invalid_host_packets();
    test_prepare_outbound_rejects_malformed_command_id();
    test_extract_options_defaults_to_single_node_small_result();
    test_prepare_outbound_accepts_all_registered_command_flood_with_roster();
    test_extract_options_accepts_all_registered_collection_without_roster();
    test_extract_options_rejects_all_registered_missing_membership_identity();
    test_command_flood_requires_collection_identity_for_responses();
    test_extract_options_accepts_all_registered_roster();
    test_extract_options_rejects_mismatched_roster();
    test_tracking_mode_keeps_single_node_on_legacy_wait();
    test_tracking_mode_skips_broadcast_no_response_wait();
    test_tracking_mode_uses_collection_for_broadcast_results();
    test_command_receive_timing_waits_until_execute_delay();
    test_command_receive_rejects_expired_or_unexecutable_delay();
    test_command_rx_duplicate_cache_blocks_replay_until_expiry();
    test_collection_initial_due_is_deterministic_and_bounded();
    test_all_node_collection_due_spreads_responses();
    test_collection_retry_round_advances_spread();
    test_append_collection_result_identity_requires_epoch();
    test_collection_records_unique_results_and_builds_eack();
    test_collection_prepares_eack_broadcast_outbound();
    test_collection_prepares_missing_list_from_roster();
    test_collection_roster_resolution_prefers_explicit_roster();
    test_collection_roster_resolution_rejects_provider_mismatch();
    test_collection_roster_resolution_leaves_all_heard_best_effort();
    test_provider_roster_feeds_missing_list_eack();
    test_collection_records_result_bundle_and_dedupes_replay();
    test_collection_return_candidates_from_direct_results();
    test_collection_snapshot_round_trips_return_hops();
    test_collection_snapshot_round_trips_closed_collection();
    test_collection_snapshot_rejects_corrupt_without_partial_restore();
    test_collection_bundle_records_return_hop();
    test_collection_return_candidates_suppress_duplicates();
    test_collection_rejects_corrupt_result_bundle();
    test_collection_rejects_wrong_result_identity();
    test_collection_rejects_missing_or_wrong_collection_epoch();
    test_extract_duration_uses_optional_tlv();
    test_extract_role_requires_valid_device_role_tlv();
    test_build_failure_result_is_host_visible();
    test_pending_command_completes_on_matching_result();
    test_pending_command_expires_with_original_context();
    test_pending_command_expiry_handles_ms_wrap();
    test_pending_survey_prepare_completes_on_command_result();
    test_pending_command_rejects_second_start();
    return 0;
}
