#include "gateway_command.h"

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

static void test_prepare_outbound_accepts_all_registered_command_flood(void)
{
    uint8_t payload[96];
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
    host.payload_len = (uint8_t)payload_len;

    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) == PROTO_OK);
    assert(options.scope == CMD_SCOPE_ALL_REGISTERED);
    assert(options.response_mode == CMD_RESPONSE_SMALL_RESULT);
    assert(options.command_seq == 1001u);
    assert(options.flood_epoch_id == 2002u);
    assert(options.membership_epoch == 3u);
    assert(options.expected_node_count == 12u);
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
    assert(out.payload_len == payload_len);
    assert(memcmp(out.payload, payload, payload_len) == 0);
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
    test_prepare_outbound_accepts_all_registered_command_flood();
    test_command_flood_requires_collection_identity_for_responses();
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
