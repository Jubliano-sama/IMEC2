#include "gateway_command.h"

#include "discovery_assignment.h"
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

static uint32_t require_tlv_u32(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint32_t));
    return proto_get_u32_le(value);
}

static uint16_t require_tlv_u16(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint16_t));
    return proto_get_u16_le(value);
}

static uint8_t require_tlv_u8(const uint8_t *payload, size_t payload_len, uint8_t type)
{
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;

    assert(tlv_find(payload, payload_len, type, &value, &value_len) == PROTO_OK);
    assert(value_len == sizeof(uint8_t));
    return value[0];
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

static void assert_collection_result_id_equal(
    const struct gateway_collection_state *collection,
    const struct gateway_collection_result_id *actual,
    const struct command_result_id *expected)
{
    assert(collection->gateway_id == expected->gateway_id);
    assert(collection->gateway_epoch == expected->gateway_epoch);
    assert(collection->command_seq == expected->command_seq);
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

static void make_crc16_colliding_collection_result_payloads(
    uint8_t *first,
    size_t first_cap,
    size_t *first_len,
    uint8_t *second,
    size_t second_cap,
    size_t *second_len,
    const struct command_result_id *id,
    uint32_t collection_epoch_id)
{
    *first_len = 0u;
    *second_len = 0u;
    assert(gateway_command_append_collection_result_identity(
               first,
               first_cap,
               first_len,
               id,
               collection_epoch_id) == PROTO_OK);
    assert(mesh_append_command_result(first,
                                      first_cap,
                                      first_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      0u) == PROTO_OK);
    assert(tlv_append_u16(first,
                          first_cap,
                          first_len,
                          TLV_FW_VERSION,
                          UINT16_C(0x3037)) == PROTO_OK);

    assert(gateway_command_append_collection_result_identity(
               second,
               second_cap,
               second_len,
               id,
               collection_epoch_id) == PROTO_OK);
    assert(mesh_append_command_result(second,
                                      second_cap,
                                      second_len,
                                      CMD_GET_STATUS,
                                      COMMAND_OK,
                                      1u) == PROTO_OK);
    assert(tlv_append_u16(second,
                          second_cap,
                          second_len,
                          TLV_FW_VERSION,
                          0u) == PROTO_OK);

    assert(*first_len == 61u);
    assert(*second_len == *first_len);
    assert(memcmp(first, second, *first_len) != 0);
    assert(proto_crc16_ccitt_false(first, *first_len) == UINT16_C(0xfadf));
    assert(proto_crc16_ccitt_false(second, *second_len) == UINT16_C(0xfadf));
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

static void wrap_collection_result_bundle(uint8_t *payload,
                                          size_t payload_cap,
                                          size_t *payload_len,
                                          struct result_bundle_header *bundle,
                                          struct proto_packet *bundle_packet,
                                          const uint8_t *records,
                                          size_t records_len)
{
    *payload_len = 0u;
    bundle->bundle_crc = proto_crc16_ccitt_false(records, records_len);
    assert(result_bundle_header_append_tlvs(payload,
                                            payload_cap,
                                            payload_len,
                                            bundle) == PROTO_OK);
    assert(payload_cap - *payload_len >= records_len);
    memcpy(&payload[*payload_len], records, records_len);
    *payload_len += records_len;
    bundle_packet->payload_len = (uint16_t)*payload_len;
}

static void make_two_record_collection_bundle(
    const struct command_result_id *record_id_a,
    const struct command_result_id *payload_id_a,
    const struct command_result_id *record_id_b,
    const struct command_result_id *payload_id_b,
    uint16_t bundle_id,
    uint8_t *bundle_payload,
    size_t bundle_payload_cap,
    size_t *bundle_payload_len,
    struct proto_packet *bundle_packet)
{
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = bundle_id,
        .record_count = 2u,
    };
    uint8_t result_payload_a[96];
    uint8_t result_payload_b[96];
    uint8_t records[192];
    size_t result_payload_a_len = 0u;
    size_t result_payload_b_len = 0u;
    size_t records_len = 0u;

    make_collection_result_payload(result_payload_a,
                                   sizeof(result_payload_a),
                                   &result_payload_a_len,
                                   payload_id_a,
                                   3003u);
    make_collection_result_payload(result_payload_b,
                                   sizeof(result_payload_b),
                                   &result_payload_b_len,
                                   payload_id_b,
                                   3003u);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    record_id_a,
                                    result_payload_a,
                                    result_payload_a_len);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    record_id_b,
                                    result_payload_b,
                                    result_payload_b_len);

    *bundle_packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = bundle_id,
        .ttl = 1u,
    };
    wrap_collection_result_bundle(bundle_payload,
                                  bundle_payload_cap,
                                  bundle_payload_len,
                                  &bundle,
                                  bundle_packet,
                                  records,
                                  records_len);
}

static void make_collection_bundle_from_ids(
    const struct command_result_id *ids,
    size_t id_count,
    uint16_t bundle_id,
    uint8_t *bundle_payload,
    size_t bundle_payload_cap,
    size_t *bundle_payload_len,
    struct proto_packet *bundle_packet)
{
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = bundle_id,
        .record_count = (uint8_t)id_count,
    };
    uint8_t result_payload[96];
    uint8_t records[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t records_len = 0u;

    assert(ids != NULL);
    assert(id_count > 0u);
    assert(id_count <= COLLECTION_BUNDLE_MAX_RECORDS);
    for (size_t i = 0u; i < id_count; i++) {
        size_t result_payload_len = 0u;

        make_collection_result_payload(result_payload,
                                       sizeof(result_payload),
                                       &result_payload_len,
                                       &ids[i],
                                       3003u);
        append_collection_result_record(records,
                                        sizeof(records),
                                        &records_len,
                                        &ids[i],
                                        result_payload,
                                        result_payload_len);
    }

    *bundle_packet = (struct proto_packet) {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = bundle_id,
        .ttl = 1u,
    };
    wrap_collection_result_bundle(bundle_payload,
                                  bundle_payload_cap,
                                  bundle_payload_len,
                                  &bundle,
                                  bundle_packet,
                                  records,
                                  records_len);
}

static size_t first_result_record_offset(const uint8_t *payload,
                                         size_t payload_len)
{
    size_t offset = 0u;

    while (offset < payload_len) {
        assert(payload_len - offset >= 2u);
        assert(payload_len - offset - 2u >= payload[offset + 1u]);
        if (payload[offset] == TLV_RESULT_RECORD) {
            return offset;
        }
        offset += 2u + payload[offset + 1u];
    }
    assert(false);
    return 0u;
}

static void start_test_rostered_collection(
    struct gateway_collection_state *collection,
    const uint64_t *roster,
    size_t roster_count)
{
    assert(roster_count != 0u);
    assert(roster_count <= GATEWAY_COLLECTION_RESULT_CACHE_SIZE);
    assert(gateway_collection_start(collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    (uint16_t)roster_count,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_set_expected_roster(collection,
                                                  roster,
                                                  roster_count) == PROTO_OK);
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
    assert(out.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(out.packet.flags == FLAG_DIAGNOSTIC);
    assert(gateway_command_transport_mode_from_outbound(&out) ==
           GATEWAY_COMMAND_TRANSPORT_UNICAST_TRACKED);
    assert(out.payload_len == payload_len);
    assert(memcmp(out.payload, payload, payload_len) == 0);
}

static void test_prepare_outbound_preserves_host_session_sequence_and_normalizes_ttl(void)
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
    assert(out.packet.ttl == FLOOD_EPOCH_GLOBAL_TTL);
    assert(out.packet.flags == 0u);
}

static void test_prepare_outbound_uses_command_class_ttl_for_every_host_value(void)
{
    static const struct {
        enum command_id command_id;
        uint8_t expected_ttl;
    } command_cases[] = {
        {CMD_PING, FLOOD_EPOCH_GLOBAL_TTL},
        {CMD_ASSIGN_DISCOVERY_SLOTS, FLOOD_EPOCH_GLOBAL_TTL},
    };
    static const uint8_t host_ttls[] = {
        0u,
        FLOOD_EPOCH_GLOBAL_TTL,
        1u,
        UINT8_MAX,
    };

    for (size_t command_index = 0u;
         command_index < sizeof(command_cases) / sizeof(command_cases[0]);
         command_index++) {
        for (size_t ttl_index = 0u;
             ttl_index < sizeof(host_ttls) / sizeof(host_ttls[0]);
             ttl_index++) {
            uint8_t payload[16];
            size_t payload_len = 0u;
            struct proto_packet host = {
                .msg_type = MSG_COMMAND,
                .dst_id = ANCHOR_ID_TEST,
                .session_id = 77u,
                .seq = 22u,
                .ttl = host_ttls[ttl_index],
            };
            struct mesh_outbound out = {0};
            enum command_id decoded_command_id = CMD_VENDOR_BASE;

            make_command_payload(payload,
                                 sizeof(payload),
                                 &payload_len,
                                 command_cases[command_index].command_id);
            host.payload_len = (uint8_t)payload_len;
            assert(gateway_command_prepare_outbound(
                       &host,
                       payload,
                       payload_len,
                       GATEWAY_ID_TEST,
                       555u,
                       10u,
                       &out,
                       &decoded_command_id) == PROTO_OK);
            assert(decoded_command_id ==
                   command_cases[command_index].command_id);
            assert(out.packet.ttl ==
                   command_cases[command_index].expected_ttl);
        }
    }
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

static void test_duplicate_command_singletons_are_rejected(void)
{
    struct gateway_command_options options;
    enum command_id command_id = CMD_VENDOR_BASE;
    enum device_role role = ROLE_CLICKER;
    uint8_t payload[64];
    size_t payload_len = 0u;
    uint32_t duration_ms = 0u;

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    assert(tlv_append_u16(payload,
                          sizeof(payload),
                          &payload_len,
                          TLV_COMMAND_ID,
                          CMD_GET_STATUS) == PROTO_OK);
    assert(gateway_command_extract_id(payload,
                                      payload_len,
                                      &command_id) ==
           PROTO_ERR_MALFORMED);

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_SINGLE_NODE) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_ALL_HEARD) == PROTO_OK);
    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) ==
           PROTO_ERR_MALFORMED);

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_PING);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_SEQ, 10u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_SEQ, 11u) == PROTO_OK);
    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DEVICE_ROLE, ROLE_ANCHOR) == PROTO_OK);
    assert(tlv_append_u8(payload, sizeof(payload), &payload_len,
                         TLV_DEVICE_ROLE, ROLE_GATEWAY) == PROTO_OK);
    assert(gateway_command_extract_role(payload, payload_len, &role) ==
           PROTO_ERR_MALFORMED);

    payload_len = 0u;
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_DURATION_MS, 100u) == PROTO_OK);
    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_DURATION_MS, 200u) == PROTO_OK);
    assert(gateway_command_extract_duration_ms(
               payload, payload_len, 50u, &duration_ms) ==
           PROTO_ERR_MALFORMED);
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

static void test_extract_options_rejects_unsupported_group_scope(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    struct gateway_command_options options = {0};

    make_command_payload(payload, sizeof(payload), &payload_len, CMD_GET_STATUS);
    assert(tlv_append_u8(payload,
                         sizeof(payload),
                         &payload_len,
                         TLV_COMMAND_SCOPE,
                         CMD_SCOPE_GROUP) == PROTO_OK);
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

    /*
     * No group identity or membership proof exists on the wire. Accepting
     * this scope would therefore turn a nominally targeted command into an
     * all-anchor broadcast.
     */
    assert(gateway_command_extract_options(payload,
                                           payload_len,
                                           &options) ==
           PROTO_ERR_MALFORMED);
}

static void test_command_scope_applies_to_explicit_and_derived_membership(void)
{
    const uint32_t assignment_epoch = UINT32_C(0x12345678);
    const uint16_t membership_epoch =
        discovery_assignment_membership_epoch(assignment_epoch);
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_REGISTERED,
        .membership_epoch = membership_epoch,
        .expected_node_count = 2u,
    };

    options.expected_node_id_count = 2u;
    options.expected_node_ids[0] = UINT64_C(0x2222333344445555);
    options.expected_node_ids[1] = UINT64_C(0x6666777788889999);
    assert(!gateway_command_applies_to_node(&options,
                                            ANCHOR_ID_TEST,
                                            true,
                                            assignment_epoch));

    /*
     * The gateway has already bound an explicit roster to durable current
     * membership. Local ID inclusion is therefore sufficient even while the
     * anchor is restoring its assignment epoch.
     */
    options.expected_node_ids[1] = ANCHOR_ID_TEST;
    assert(gateway_command_applies_to_node(&options,
                                           ANCHOR_ID_TEST,
                                           false,
                                           0u));
    assert(gateway_command_applies_to_node(&options,
                                           ANCHOR_ID_TEST,
                                           true,
                                           assignment_epoch + 1u));
    assert(gateway_command_applies_to_node(&options,
                                           ANCHOR_ID_TEST,
                                           true,
                                           assignment_epoch));

    options.expected_node_id_count = 0u;
    assert(!gateway_command_applies_to_node(&options,
                                            ANCHOR_ID_TEST,
                                            false,
                                            assignment_epoch));
    assert(!gateway_command_applies_to_node(&options,
                                            ANCHOR_ID_TEST,
                                            true,
                                            assignment_epoch + 1u));
    assert(gateway_command_applies_to_node(&options,
                                           ANCHOR_ID_TEST,
                                           true,
                                           assignment_epoch));

    options.scope = CMD_SCOPE_GROUP;
    assert(!gateway_command_applies_to_node(&options,
                                            ANCHOR_ID_TEST,
                                            true,
                                            assignment_epoch));

    options.scope = CMD_SCOPE_ALL_HEARD;
    assert(gateway_command_applies_to_node(&options,
                                           ANCHOR_ID_TEST,
                                           false,
                                           0u));
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
    assert(out.payload_len > payload_len);
    assert(memcmp(out.payload, payload, payload_len) == 0);
    assert(require_tlv_u32(out.payload,
                           out.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_MAX_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_MAX_MS);
    assert(require_tlv_u16(out.payload,
                           out.payload_len,
                           TLV_FLOOD_RANDOM_BACKOFF_SLOT_MS) ==
           FLOOD_RANDOM_BACKOFF_DEFAULT_SLOT_MS);
    assert(require_tlv_u8(out.payload, out.payload_len, TLV_FLOOD_RETRY_COUNT) ==
           FLOOD_DEFAULT_RETRY_COUNT);
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

static void test_command_rx_duplicate_history_blocks_replay_after_delivery_expiry(void)
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
    /*
     * Delivery expiry bounds fresh admission, but it cannot make a committed
     * side-effect identity executable again.
     */
    assert(gateway_command_rx_duplicate_seen(&cache, options.command_seq, 2900u));
    assert(gateway_command_rx_duplicate_seen(&cache, options.command_seq, 90000u));
}

static void test_command_rx_high_watermark_retains_more_than_legacy_capacity(void)
{
    struct gateway_command_rx_duplicate_cache cache = {0};
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_HEARD,
        .response_mode = CMD_RESPONSE_NONE,
        .flood_epoch_id = 88u,
        .command_expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
    };
    const uint32_t first_command_seq = 1001u;
    const size_t command_count = 6u;

    /*
     * The legacy four-entry replacement cache made a still-unexpired command
     * executable again after four later commands. Exercise beyond that old
     * capacity and beyond the intermediate 64-bit bitset while every command
     * remains inside the default one-day lifetime.
     */
    for (size_t i = 0u; i < command_count; i++) {
        options.command_seq = first_command_seq + (uint32_t)i;
        packet.session_id = options.command_seq;
        packet.seq = (uint16_t)(i + 1u);
        assert(!gateway_command_rx_duplicate_seen(&cache,
                                                  options.command_seq,
                                                  1000u + (uint32_t)i));
        gateway_command_rx_duplicate_store(&cache,
                                           &packet,
                                           &options,
                                           1000u + (uint32_t)i);
    }

    assert(gateway_command_rx_duplicate_seen(&cache,
                                             first_command_seq,
                                             2000u));

    for (size_t i = command_count; i < 66u; i++) {
        options.command_seq = first_command_seq + (uint32_t)i;
        packet.session_id = options.command_seq;
        packet.seq = (uint16_t)(i + 1u);
        assert(!gateway_command_rx_duplicate_seen(&cache,
                                                  options.command_seq,
                                                  2000u + (uint32_t)i));
        gateway_command_rx_duplicate_store(&cache,
                                           &packet,
                                           &options,
                                           2000u + (uint32_t)i);
    }
    assert(gateway_command_rx_duplicate_seen(&cache,
                                             first_command_seq,
                                             3000u));
}

static void test_command_rx_high_watermark_rejects_uncommitted_older_gap(void)
{
    struct gateway_command_rx_duplicate_cache cache = {0};
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_HEARD,
        .response_mode = CMD_RESPONSE_NONE,
        .command_seq = 42u,
        .flood_epoch_id = 42u,
        .command_expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = 42u,
        .seq = 42u,
    };

    /* Sequence 41 was deferred and never committed before 42 executed. */
    assert(!gateway_command_rx_duplicate_seen(&cache, 41u, 1000u));
    gateway_command_rx_duplicate_store(&cache, &packet, &options, 1001u);
    assert(gateway_command_rx_duplicate_seen(&cache, 42u, 1002u));
    assert(gateway_command_rx_duplicate_seen(&cache, 41u, 1002u));
    assert(gateway_command_rx_duplicate_seen(&cache, 1u, 1002u));
    assert(!gateway_command_rx_duplicate_seen(&cache, 43u, 1002u));

    /* RFC 1982 half-range ambiguity is neither newer nor replayable. */
    assert(gateway_command_rx_duplicate_seen(
        &cache, 42u + UINT32_C(0x80000000), 1002u));
}

static void test_command_rx_high_watermark_accepts_wrap_newer_only(void)
{
    struct gateway_command_rx_duplicate_cache cache = {0};
    struct gateway_command_options options = {
        .scope = CMD_SCOPE_ALL_HEARD,
        .response_mode = CMD_RESPONSE_NONE,
        .command_seq = UINT32_MAX,
        .flood_epoch_id = UINT32_MAX,
        .command_expiry_s = COMMAND_RESULT_EXPIRY_DEFAULT_S,
        .flood_required = true,
    };
    struct proto_packet packet = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = MESH_BROADCAST_ID,
        .session_id = UINT32_MAX,
        .seq = UINT16_MAX,
    };

    gateway_command_rx_duplicate_store(&cache, &packet, &options, 1000u);
    assert(!gateway_command_rx_duplicate_seen(&cache, 1u, 1001u));

    options.command_seq = 1u;
    options.flood_epoch_id = 1u;
    packet.session_id = 1u;
    packet.seq = 1u;
    gateway_command_rx_duplicate_store(&cache, &packet, &options, 1001u);
    assert(gateway_command_rx_duplicate_seen(&cache, UINT32_MAX, 1002u));
    assert(gateway_command_rx_duplicate_seen(&cache, 1u, 1002u));
    assert(!gateway_command_rx_duplicate_seen(&cache, 2u, 1002u));
    assert(gateway_command_rx_duplicate_seen(
        &cache, 1u + UINT32_C(0x80000000), 1002u));
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
    assert(collection.eack_sequence == 2u);
    assert(collection.next_retry_spread_ms == COLLECTION_RETRY_ROUND_1_MS);
    assert(gateway_collection_advance_retry_round(&collection) == PROTO_OK);
    assert(collection.retry_round == 2u);
    assert(collection.eack_sequence == 3u);
    assert(collection.next_retry_spread_ms == COLLECTION_RETRY_ROUND_2_MS);
}

static void test_collection_eack_sequence_wraps_nonzero_after_dedup_horizon(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_eack decoded;
    struct mesh_outbound out;
    uint16_t observed[3];

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    12u,
                                    UINT8_MAX,
                                    COLLECTION_RETRY_ROUND_STEADY_MS) == PROTO_OK);
    collection.eack_sequence = UINT16_MAX - 1u;

    for (size_t i = 0u; i < 3u; i++) {
        assert(gateway_collection_prepare_eack_outbound(
                   &collection,
                   EACK_FORMAT_EXPLICIT_RECEIVED_LIST,
                   &out) == PROTO_OK);
        assert(gateway_collection_eack_packet_validate(&out.packet,
                                                       out.payload,
                                                       out.payload_len,
                                                       &decoded) == PROTO_OK);
        observed[i] = out.packet.seq;
        assert(observed[i] != 0u);
        assert(decoded.packet_sequence == observed[i]);
        assert(decoded.received_count == collection.received_count);
        assert(decoded.collection_open == collection.collection_open);
        if (i != 2u) {
            assert(gateway_collection_advance_retry_round(&collection) == PROTO_OK);
        }
    }

    assert(observed[0] == UINT16_MAX - 1u);
    assert(observed[1] == UINT16_MAX);
    assert(observed[2] == 1u);
    assert(observed[0] != observed[1]);
    assert(observed[1] != observed[2]);
    assert(collection.retry_round == UINT8_MAX);
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
    assert(collection.eack_pending);
    assert(GATEWAY_COLLECTION_RESULT_CACHE_SIZE == 50u);
    assert(sizeof(collection) == GATEWAY_COLLECTION_STATE_SIZE);

    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 1u);
    assert(collection.collection_open);
    assert(collection.eack_pending);
    assert(gateway_collection_contains_result(&collection, &id_a));
    assert_collection_result_id_equal(&collection, &collection.results[0].id, &id_a);
    {
        uint8_t expected_digest[SEMANTIC_DIGEST_SHA256_LEN];

        assert(semantic_digest_sha256(payload_a,
                                      payload_a_len,
                                      expected_digest));
        assert(semantic_digest_equal(collection.results[0].payload_digest,
                                     expected_digest,
                                     sizeof(expected_digest)));
    }
    assert(collection.results[0].payload_len == payload_a_len);

    duplicate = false;
    assert(gateway_collection_record_result(&collection,
                                            &result_a,
                                            payload_a,
                                            payload_a_len,
                                            &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(collection.received_count == 1u);

    collection.eack_pending = false;
    assert(gateway_collection_record_result(&collection,
                                            &result_b,
                                            payload_b,
                                            payload_b_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 2u);
    assert(!collection.collection_open);
    assert(collection.eack_pending);

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
    assert(eack.packet_sequence == 2u);
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

static void test_collection_rejects_crc16_collision(void)
{
    const struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct gateway_collection_state collection;
    struct proto_packet result;
    uint8_t first[96];
    uint8_t second[96];
    size_t first_len;
    size_t second_len;
    bool duplicate = false;

    make_crc16_colliding_collection_result_payloads(
        first,
        sizeof(first),
        &first_len,
        second,
        sizeof(second),
        &second_len,
        &id,
        3003u);
    result = make_collection_result_packet(&id, first_len);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    1u,
                                    COLLECTION_RETRY_ROUND_1_MS) == PROTO_OK);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            first,
                                            first_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    duplicate = false;
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            second,
                                            second_len,
                                            &duplicate) ==
           PROTO_ERR_MALFORMED);
    assert(!duplicate);
    assert(collection.received_count == 1u);

}

static void test_collection_bundle_rejects_staged_crc16_collision(void)
{
    const struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct gateway_collection_state collection;
    struct result_bundle_header bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = 41u,
        .record_count = 2u,
    };
    struct proto_packet bundle_packet = {
        .msg_type = MSG_RESULT_BUNDLE,
        .src_id = 0x2222333344445555ull,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 1001u,
        .seq = 41u,
        .ttl = 1u,
    };
    uint8_t first[96];
    uint8_t second[96];
    uint8_t records[256];
    uint8_t bundle_payload[384];
    size_t first_len;
    size_t second_len;
    size_t records_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted = UINT16_MAX;
    uint16_t duplicates = UINT16_MAX;

    make_crc16_colliding_collection_result_payloads(
        first,
        sizeof(first),
        &first_len,
        second,
        sizeof(second),
        &second_len,
        &id,
        3003u);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id,
                                    first,
                                    first_len);
    append_collection_result_record(records,
                                    sizeof(records),
                                    &records_len,
                                    &id,
                                    second,
                                    second_len);
    wrap_collection_result_bundle(bundle_payload,
                                  sizeof(bundle_payload),
                                  &bundle_payload_len,
                                  &bundle,
                                  &bundle_packet,
                                  records,
                                  records_len);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    1u,
                                    COLLECTION_RETRY_ROUND_1_MS) == PROTO_OK);
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted,
                                            &duplicates) ==
           PROTO_ERR_MALFORMED);
    assert(collection.received_count == 0u);
    assert(collection.collection_open);
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
    assert(out.packet.seq == 3u);
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
    assert(decoded.packet_sequence == 3u);
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
    assert(out.packet.seq == 3u);
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

static void test_collection_roster_resolution_binds_explicit_roster_to_membership(void)
{
    const uint64_t provider_ids[] = {
        0x5555666677778888ull,
        ANCHOR_ID_TEST,
    };
    const uint64_t mismatched_provider_ids[] = {
        0x9999000000000001ull,
        ANCHOR_ID_TEST,
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
                                                        7u,
                                                        provider_ids,
                                                        2u) == PROTO_OK);

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

    /*
     * An explicit collection roster is an ordering/filter over the frozen
     * membership snapshot. It must never introduce an unregistered node.
     */
    assert(gateway_membership_set_roster_preserve_order(
               &membership,
               7u,
               mismatched_provider_ids,
               2u) == PROTO_OK);
    assert(gateway_command_resolve_collection_roster(
               &options,
               &membership,
               resolved,
               sizeof(resolved) / sizeof(resolved[0]),
               &resolved_count,
               &source) == PROTO_ERR_STALE);
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

static void test_collection_roster_binding_validates_exact_membership(void)
{
    const uint64_t roster[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
    };
    uint64_t invalid_roster[2];
    struct gateway_collection_state collection;
    struct gateway_collection_state before;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    before = collection;
    assert(gateway_collection_set_expected_roster(&collection,
                                                  roster,
                                                  1u) == PROTO_ERR_MALFORMED);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);

    invalid_roster[0] = roster[0];
    invalid_roster[1] = roster[0];
    assert(gateway_collection_set_expected_roster(&collection,
                                                  invalid_roster,
                                                  2u) == PROTO_ERR_MALFORMED);
    invalid_roster[1] = 0u;
    assert(gateway_collection_set_expected_roster(&collection,
                                                  invalid_roster,
                                                  2u) == PROTO_ERR_MALFORMED);
    invalid_roster[1] = GATEWAY_ID_TEST;
    assert(gateway_collection_set_expected_roster(&collection,
                                                  invalid_roster,
                                                  2u) == PROTO_ERR_MALFORMED);

    assert(gateway_collection_set_expected_roster(&collection,
                                                  roster,
                                                  2u) == PROTO_OK);
    assert(collection.expected_node_id_count == 2u);
    assert(memcmp(collection.expected_node_ids,
                  roster,
                  sizeof(roster)) == 0);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_set_expected_roster(&collection, NULL, 0u) == PROTO_OK);
    assert(collection.expected_node_id_count == 0u);
}

static void test_collection_roster_filters_direct_results(void)
{
    const uint64_t roster[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
    };
    struct gateway_collection_state collection;
    struct command_result_id id = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = 0x4444555566667777ull,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct proto_packet result;
    uint8_t payload[96];
    uint8_t changed_payload[96];
    size_t payload_len = 0u;
    bool duplicate = true;

    start_test_rostered_collection(&collection, roster, 2u);
    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_NOT_FOUND);
    assert(collection.received_count == 0u);

    id.node_id = roster[0];
    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 1u);

    duplicate = false;
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_OK);
    assert(duplicate);

    memcpy(changed_payload, payload, payload_len);
    changed_payload[payload_len - 1u] ^= 0x01u;
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            changed_payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_MALFORMED);

    id.node_boot_counter++;
    id.result_seq++;
    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_ERR_MALFORMED);

    id.node_id = roster[1];
    id.node_boot_counter++;
    id.result_seq++;
    make_collection_result_payload(payload, sizeof(payload), &payload_len, &id, 3003u);
    result = make_collection_result_packet(&id, payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &result,
                                            payload,
                                            payload_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(collection.received_count == 2u);
    assert(!collection.collection_open);
}

static void test_collection_roster_filters_bundles_atomically(void)
{
    const uint64_t roster[] = {
        ANCHOR_ID_TEST,
        0x3333444455556666ull,
    };
    struct gateway_collection_state collection;
    struct gateway_collection_state before;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = roster[0],
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct command_result_id unknown = id_a;
    struct result_bundle_header conflicting_bundle = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .collection_epoch_id = 3003u,
        .bundle_id = 73u,
        .record_count = 1u,
    };
    struct proto_packet bundle_packet;
    uint8_t changed_payload[96];
    uint8_t conflicting_record[128];
    uint8_t bundle_payload[256];
    size_t changed_payload_len = 0u;
    size_t conflicting_record_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0x1111u;
    uint16_t duplicate_count = 0x2222u;

    id_b.node_id = roster[1];
    id_b.node_boot_counter++;
    id_b.result_seq++;
    unknown.node_id = 0x4444555566667777ull;
    unknown.node_boot_counter += 2u;
    unknown.result_seq += 2u;

    start_test_rostered_collection(&collection, roster, 2u);
    make_two_record_collection_bundle(&id_a,
                                      &id_a,
                                      &unknown,
                                      &unknown,
                                      70u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);
    before = collection;
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_NOT_FOUND);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(accepted_count == 0x1111u);
    assert(duplicate_count == 0x2222u);

    unknown = id_a;
    unknown.node_boot_counter++;
    unknown.result_seq++;
    make_two_record_collection_bundle(&id_a,
                                      &id_a,
                                      &unknown,
                                      &unknown,
                                      71u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_MALFORMED);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);

    make_two_record_collection_bundle(&id_a,
                                      &id_a,
                                      &id_b,
                                      &id_b,
                                      72u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_OK);
    assert(accepted_count == 2u);
    assert(duplicate_count == 0u);
    assert(!collection.collection_open);

    collection.eack_pending = false;
    before = collection;
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_OK);
    assert(accepted_count == 0u);
    assert(duplicate_count == 2u);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);

    make_collection_result_payload(changed_payload,
                                   sizeof(changed_payload),
                                   &changed_payload_len,
                                   &id_a,
                                   3003u);
    assert(tlv_append_u8(changed_payload,
                         sizeof(changed_payload),
                         &changed_payload_len,
                         TLV_MESH_TEST_PADDING,
                         0xA5u) == PROTO_OK);
    append_collection_result_record(conflicting_record,
                                    sizeof(conflicting_record),
                                    &conflicting_record_len,
                                    &id_a,
                                    changed_payload,
                                    changed_payload_len);
    bundle_packet.seq = conflicting_bundle.bundle_id;
    wrap_collection_result_bundle(bundle_payload,
                                  sizeof(bundle_payload),
                                  &bundle_payload_len,
                                  &conflicting_bundle,
                                  &bundle_packet,
                                  conflicting_record,
                                  conflicting_record_len);
    accepted_count = 0x3333u;
    duplicate_count = 0x4444u;
    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_MALFORMED);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(accepted_count == 0x3333u);
    assert(duplicate_count == 0x4444u);
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
    struct gateway_collection_state before_duplicate_replay;
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

    collection.eack_pending = false;
    before_duplicate_replay = collection;
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
    assert(!collection.eack_pending);
    assert(memcmp(&collection,
                  &before_duplicate_replay,
                  sizeof(collection)) == 0);
}

static void test_collection_bundle_commits_mixed_new_and_duplicate_results(void)
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
    struct proto_packet direct_result;
    struct proto_packet bundle_packet;
    uint8_t direct_payload[96];
    uint8_t bundle_payload[256];
    size_t direct_payload_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 99u;
    uint16_t duplicate_count = 99u;
    bool duplicate = false;

    id_b.node_id++;
    id_b.node_boot_counter++;
    id_b.result_seq++;
    make_collection_result_payload(direct_payload,
                                   sizeof(direct_payload),
                                   &direct_payload_len,
                                   &id_a,
                                   3003u);
    direct_result = make_collection_result_packet(&id_a, direct_payload_len);
    make_two_record_collection_bundle(&id_b,
                                      &id_b,
                                      &id_b,
                                      &id_b,
                                      60u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    3u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    assert(gateway_collection_record_result(&collection,
                                            &direct_result,
                                            direct_payload,
                                            direct_payload_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    assert(gateway_collection_record_bundle_from_hop(&collection,
                                                     &bundle_packet,
                                                     bundle_payload,
                                                     bundle_payload_len,
                                                     bundle_packet.src_id,
                                                     &accepted_count,
                                                     &duplicate_count) == PROTO_OK);
    assert(accepted_count == 1u);
    assert(duplicate_count == 1u);
    assert(collection.received_count == 2u);
    assert(collection.collection_open);
    assert(collection.eack_pending);
    assert(gateway_collection_contains_result(&collection, &id_a));
    assert(gateway_collection_contains_result(&collection, &id_b));
}

static void test_collection_bundle_malformed_later_record_rolls_back(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_state before;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct command_result_id wrong_payload_id = id_a;
    struct proto_packet bundle_packet;
    uint8_t bundle_payload[256];
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0x1111u;
    uint16_t duplicate_count = 0x2222u;

    id_b.node_id++;
    id_b.node_boot_counter++;
    id_b.result_seq++;
    wrong_payload_id = id_b;
    wrong_payload_id.result_seq++;
    make_two_record_collection_bundle(&id_a,
                                      &id_a,
                                      &id_b,
                                      &wrong_payload_id,
                                      61u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    4u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    before = collection;

    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_MALFORMED);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(accepted_count == 0x1111u);
    assert(duplicate_count == 0x2222u);
}

static void test_collection_rejects_expected_count_beyond_capacity(void)
{
    struct gateway_collection_state collection;

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    GATEWAY_COLLECTION_RESULT_CACHE_SIZE + 2u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_ERR_ARG);
}

static void test_collection_bundle_stale_later_record_rolls_back(void)
{
    struct gateway_collection_state collection;
    struct gateway_collection_state before;
    struct command_result_id id_a = {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    struct command_result_id id_b = id_a;
    struct proto_packet bundle_packet;
    uint8_t bundle_payload[256];
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0x5555u;
    uint16_t duplicate_count = 0x6666u;

    id_b.node_id++;
    id_b.node_boot_counter++;
    id_b.result_seq++;
    make_two_record_collection_bundle(&id_a,
                                      &id_a,
                                      &id_b,
                                      &id_b,
                                      63u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    1u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    before = collection;

    assert(gateway_collection_record_bundle(&collection,
                                            &bundle_packet,
                                            bundle_payload,
                                            bundle_payload_len,
                                            &accepted_count,
                                            &duplicate_count) == PROTO_ERR_STALE);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(accepted_count == 0x5555u);
    assert(duplicate_count == 0x6666u);
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

static void test_command_budget_is_optional_bounded_and_phase_aware(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    uint32_t budget_ms = 0u;
    bool explicit_budget = true;

    assert(mesh_append_command_id(payload, sizeof(payload), &payload_len,
                                  CMD_ASSIGN_DISCOVERY_SLOTS) == PROTO_OK);
    assert(gateway_command_extract_budget_ms(
               payload, payload_len, 90000u, &budget_ms,
               &explicit_budget) == PROTO_OK);
    assert(budget_ms == 90000u);
    assert(!explicit_budget);
    assert(DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS <=
           GATEWAY_COMMAND_BUDGET_MAX_MS);
    assert(gateway_command_extract_budget_ms(
               payload, payload_len,
               DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS,
               &budget_ms, &explicit_budget) == PROTO_OK);
    assert(budget_ms == DISCOVERY_ASSIGNMENT_OPERATION_DEFAULT_BUDGET_MS);
    assert(!explicit_budget);

    assert(tlv_append_u32(payload, sizeof(payload), &payload_len,
                          TLV_COMMAND_BUDGET_MS, 15000u) == PROTO_OK);
    assert(gateway_command_extract_budget_ms(
               payload, payload_len, 90000u, &budget_ms,
               &explicit_budget) == PROTO_OK);
    assert(budget_ms == 15000u);
    assert(explicit_budget);

    payload[payload_len - 4u] = 0xe7u;
    payload[payload_len - 3u] = 0x03u;
    payload[payload_len - 2u] = 0u;
    payload[payload_len - 1u] = 0u;
    assert(gateway_command_extract_budget_ms(
               payload, payload_len, 90000u, &budget_ms,
               &explicit_budget) == PROTO_ERR_MALFORMED);

    proto_put_u32_le(&payload[payload_len - sizeof(uint32_t)],
                     GATEWAY_COMMAND_BUDGET_MAX_MS);
    assert(gateway_command_extract_budget_ms(
               payload, payload_len, 90000u, &budget_ms,
               &explicit_budget) == PROTO_OK);
    assert(budget_ms == GATEWAY_COMMAND_BUDGET_MAX_MS);
    assert(explicit_budget);
    proto_put_u32_le(&payload[payload_len - sizeof(uint32_t)],
                     GATEWAY_COMMAND_BUDGET_MAX_MS + 1u);
    assert(gateway_command_extract_budget_ms(
               payload, payload_len, 90000u, &budget_ms,
               &explicit_budget) == PROTO_ERR_MALFORMED);

    assert(gateway_command_budget_window_ms(false, 15000u, 2u, 10000u) ==
           10000u);
    assert(gateway_command_budget_window_ms(true, 15000u, 2u, 10000u) ==
           10000u);
    assert(gateway_command_budget_window_ms(true, 15000u, 3u, 10000u) ==
           10000u);
    assert(gateway_command_budget_window_ms(true, 10000u, 1u, 10000u) ==
           10000u);
    assert(gateway_command_budget_window_ms(true, 7500u, 1u, 4750u) ==
           4750u);
    assert(gateway_command_budget_window_ms(true, 1u, 2u, 4750u) == 1u);
    assert(gateway_command_budget_window_ms(true, 0u, 1u, 4750u) == 0u);
    assert(gateway_command_budget_weighted_window_ms(
               true, 20000u, 2u, 5u, 10000u) == 10000u);
    assert(gateway_command_budget_weighted_window_ms(
               true, 12000u, 3u, 5u, 10000u) == 10000u);
    assert(gateway_command_budget_weighted_window_ms(
               false, 1u, 2u, 5u, 10000u) == 10000u);

    assert(gateway_command_budget_retry_limit(true, 15000u, 4u) == 1u);
    assert(gateway_command_budget_retry_limit(true, 30000u, 4u) == 2u);
    assert(gateway_command_budget_retry_limit(true, 60000u, 4u) == 3u);
    assert(gateway_command_budget_retry_limit(true, 90000u, 4u) == 4u);
    assert(gateway_command_budget_retry_limit(false, 1u, 4u) == 4u);
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

    assert(gateway_command_build_result(&command,
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
    assert(result.flags ==
           (FLAG_GATEWAY_ACK_REQUIRED | FLAG_ERROR | FLAG_DIAGNOSTIC));
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

static void test_build_success_result_is_not_flagged_as_error(void)
{
    struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .flags = FLAG_DIAGNOSTIC,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 456u,
        .seq = 45u,
    };
    struct proto_packet result = {0};
    uint8_t payload[32];
    size_t payload_len = 0u;

    assert(gateway_command_build_result(&command,
                                        GATEWAY_ID_TEST,
                                        CMD_FORCE_REDISCOVERY,
                                        COMMAND_OK,
                                        0u,
                                        1000u,
                                        &result,
                                        payload,
                                        sizeof(payload),
                                        &payload_len) == PROTO_OK);
    assert(result.msg_type == MSG_COMMAND_RESULT);
    assert(result.flags == (FLAG_GATEWAY_ACK_REQUIRED | FLAG_DIAGNOSTIC));
    assert(result.src_id == GATEWAY_ID_TEST);
    assert(result.dst_id == GATEWAY_ID_TEST);
    assert(result.session_id == command.session_id);
    assert(result.seq == command.seq);
    assert(result.payload_len == payload_len);
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
    struct proto_packet terminal_command = {
        .session_id = UINT32_MAX,
    };
    enum command_id terminal_command_id = CMD_VENDOR_BASE;

    assert(gateway_command_pending_start(&pending,
                                         &command,
                                         CMD_GET_STATUS,
                                         1000u,
                                         GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(pending.active);
    assert(pending.deadline_ms == 1000u + GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    assert(!gateway_command_pending_matches_result(&pending, &wrong_result));
    assert(gateway_command_pending_claim_result(
               &pending,
               &wrong_result,
               1001u,
               &terminal_command,
               &terminal_command_id) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);
    assert(terminal_command.session_id == UINT32_MAX);
    assert(terminal_command_id == CMD_VENDOR_BASE);
    assert(pending.active);

    assert(gateway_command_pending_matches_result(&pending, &result));
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               1001u,
               &terminal_command,
               &terminal_command_id) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED);
    assert(!pending.active);
    assert(terminal_command.session_id == command.session_id);
    assert(terminal_command.seq == command.seq);
    assert(terminal_command_id == CMD_GET_STATUS);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               1002u,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);
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

static void test_result_admission_respects_deferred_validation(void)
{
    const enum command_id commands[] = {CMD_PING};

    for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); i++) {
        struct gateway_command_pending pending = {0};
        struct proto_packet command = {
            .msg_type = MSG_COMMAND,
            .src_id = GATEWAY_ID_TEST,
            .dst_id = ANCHOR_ID_TEST,
            .session_id = 700u + (uint32_t)i,
            .seq = 80u + (uint16_t)i,
            .ttl = FLOOD_EPOCH_GLOBAL_TTL,
        };
        struct proto_packet result = {
            .msg_type = MSG_COMMAND_RESULT,
            .src_id = ANCHOR_ID_TEST,
            .dst_id = GATEWAY_ID_TEST,
            .session_id = command.session_id,
            .seq = command.seq,
        };
        struct proto_packet wrong_source = result;

        assert(gateway_command_pending_start(&pending,
                                             &command,
                                             commands[i],
                                             1000u,
                                             GATEWAY_COMMAND_RESULT_TIMEOUT_MS) ==
               PROTO_OK);
        assert(gateway_command_result_admit(&pending,
                                            &result,
                                            false,
                                            false) ==
               GATEWAY_COMMAND_RESULT_ACCEPT);

        wrong_source.src_id++;
        assert(gateway_command_result_admit(&pending,
                                            &wrong_source,
                                            false,
                                            false) ==
               GATEWAY_COMMAND_RESULT_IGNORE);
        assert(pending.active);

        assert(gateway_command_result_admit(&pending,
                                            &result,
                                            true,
                                            false) ==
               GATEWAY_COMMAND_RESULT_WAIT);
        assert(pending.active);
        assert(gateway_command_result_admit(&pending,
                                            &result,
                                            true,
                                            true) ==
               GATEWAY_COMMAND_RESULT_ACCEPT);

        assert(gateway_command_pending_claim_result(
                   &pending,
                   &result,
                   1001u,
                   NULL,
                   NULL) ==
               GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED);
        assert(!pending.active);
        assert(gateway_command_pending_start(&pending,
                                             &command,
                                             commands[i],
                                             1100u,
                                             GATEWAY_COMMAND_RESULT_TIMEOUT_MS) ==
               PROTO_OK);
    }
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

static void assert_pending_result_claim_boundaries(uint32_t start_ms,
                                                   uint32_t timeout_ms)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 127u,
        .seq = 47u,
        .ttl = MESH_DEFAULT_TTL,
    };
    const struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 127u,
        .seq = 47u,
    };
    const uint32_t deadline_ms = start_ms + timeout_ms;
    const struct {
        uint32_t now_ms;
        enum gateway_command_pending_result_claim expected;
    } cases[] = {
        {
            .now_ms = deadline_ms - 1u,
            .expected =
                GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED,
        },
        {
            .now_ms = deadline_ms,
            .expected =
                GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED,
        },
        {
            .now_ms = deadline_ms + 1u,
            .expected =
                GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED,
        },
    };

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct gateway_command_pending pending = {0};
        struct proto_packet terminal_command = {0};
        enum command_id terminal_command_id = CMD_VENDOR_BASE;

        assert(gateway_command_pending_start(
                   &pending,
                   &command,
                   CMD_PING,
                   start_ms,
                   timeout_ms) == PROTO_OK);
        assert(gateway_command_pending_claim_result(
                   &pending,
                   &result,
                   cases[i].now_ms,
                   &terminal_command,
                   &terminal_command_id) == cases[i].expected);
        assert(!pending.active);
        assert(terminal_command.session_id == command.session_id);
        assert(terminal_command.seq == command.seq);
        assert(terminal_command_id == CMD_PING);
        assert(!gateway_command_pending_expired(
            &pending,
            deadline_ms + 1u,
            NULL,
            NULL));
        assert(gateway_command_pending_claim_result(
                   &pending,
                   &result,
                   cases[i].now_ms,
                   NULL,
                   NULL) ==
               GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);
    }
}

static void test_pending_command_deadline_has_one_terminal_winner(void)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 128u,
        .seq = 48u,
        .ttl = MESH_DEFAULT_TTL,
    };
    const struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 128u,
        .seq = 48u,
    };
    struct proto_packet wrong_result = result;
    struct proto_packet terminal_command = {0};
    enum command_id terminal_command_id = CMD_VENDOR_BASE;
    struct gateway_command_pending pending = {0};
    const uint32_t start_ms = 1000u;
    const uint32_t deadline_ms =
        start_ms + GATEWAY_COMMAND_RESULT_TIMEOUT_MS;
    const uint32_t wrap_start_ms = UINT32_MAX - 10u;
    const uint32_t wrap_timeout_ms = 11u;

    assert_pending_result_claim_boundaries(
        start_ms, GATEWAY_COMMAND_RESULT_TIMEOUT_MS);
    assert(wrap_start_ms + wrap_timeout_ms < wrap_start_ms);
    assert_pending_result_claim_boundaries(wrap_start_ms, wrap_timeout_ms);

    assert(gateway_command_pending_start(
               &pending,
               &command,
               CMD_PING,
               start_ms,
               GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(gateway_command_pending_expired(
        &pending,
        deadline_ms,
        &terminal_command,
        &terminal_command_id));
    assert(terminal_command.session_id == command.session_id);
    assert(terminal_command_id == CMD_PING);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               deadline_ms,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);

    memset(&pending, 0, sizeof(pending));
    wrong_result.src_id++;
    assert(gateway_command_pending_start(
               &pending,
               &command,
               CMD_PING,
               start_ms,
               GATEWAY_COMMAND_RESULT_TIMEOUT_MS) == PROTO_OK);
    assert(gateway_command_pending_claim_result(
               &pending,
               &wrong_result,
               deadline_ms,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);
    assert(pending.active);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               deadline_ms,
               &terminal_command,
               &terminal_command_id) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED);
}

static void test_pending_command_absolute_deadline_is_shared(void)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 129u,
        .seq = 49u,
        .ttl = FLOOD_EPOCH_GLOBAL_TTL,
    };
    const struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 129u,
        .seq = 49u,
    };
    const uint32_t deadline_ms = 1100u;
    struct gateway_command_pending pending = {0};

    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               1007u,
               deadline_ms) == PROTO_OK);
    assert(pending.deadline_ms == deadline_ms);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               deadline_ms - 1u,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED);

    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               1099u,
               deadline_ms) == PROTO_OK);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               deadline_ms,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_EXPIRED);
    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               deadline_ms,
               deadline_ms) == PROTO_ERR_ARG);
}

static void test_result_validation_lease_serializes_deadline(void)
{
    const struct proto_packet command = {
        .msg_type = MSG_COMMAND,
        .src_id = GATEWAY_ID_TEST,
        .dst_id = ANCHOR_ID_TEST,
        .session_id = 130u,
        .seq = 50u,
        .ttl = MESH_DEFAULT_TTL,
    };
    const struct proto_packet result = {
        .msg_type = MSG_COMMAND_RESULT,
        .src_id = ANCHOR_ID_TEST,
        .dst_id = GATEWAY_ID_TEST,
        .session_id = 130u,
        .seq = 50u,
    };
    struct gateway_command_result_validation_leases leases = {0};
    struct gateway_command_pending pending = {0};
    uint32_t first_token = 0u;
    uint32_t second_token = 0u;
    const uint32_t deadline_ms = 2000u;

    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               1900u,
               deadline_ms) == PROTO_OK);
    assert(gateway_command_result_validation_acquire(
               &leases,
               &pending,
               &result,
               1899u,
               &first_token) == PROTO_ERR_NOT_FOUND);
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               1899u,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_IGNORE);
    assert(gateway_command_result_validation_acquire(
               &leases,
               &pending,
               &result,
               (uint64_t)deadline_ms - 1u,
               &first_token) == PROTO_OK);
    assert(first_token != 0u);
    assert(gateway_command_result_validation_contains(
        &leases,
        &pending,
        first_token,
        &result,
        (uint64_t)deadline_ms - 1u));
    assert(gateway_command_result_validation_blocks_timeout(
        &leases, &pending));
    assert(gateway_command_result_validation_acquire(
               &leases,
               &pending,
               &result,
               (uint64_t)deadline_ms - 1u,
               &second_token) == PROTO_OK);
    assert(second_token != 0u && second_token != first_token);
    assert(gateway_command_result_validation_release(
        &leases, first_token));
    assert(gateway_command_result_validation_blocks_timeout(
        &leases, &pending));
    assert(gateway_command_pending_claim_result(
               &pending,
               &result,
               deadline_ms - 1u,
               NULL,
               NULL) ==
           GATEWAY_COMMAND_PENDING_RESULT_CLAIM_ACCEPTED);
    gateway_command_result_validation_clear(&leases);

    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               1900u,
               deadline_ms) == PROTO_OK);
    assert(gateway_command_result_validation_acquire(
               &leases,
               &pending,
               &result,
               deadline_ms,
               &first_token) == PROTO_ERR_NOT_FOUND);
    assert(!gateway_command_result_validation_blocks_timeout(
        &leases, &pending));
    assert(gateway_command_pending_expired(
        &pending, deadline_ms, NULL, NULL));

    assert(gateway_command_pending_start_until(
               &pending,
               &command,
               CMD_PING,
               1900u,
               deadline_ms) == PROTO_OK);
    assert(gateway_command_result_validation_acquire(
               &leases,
               &pending,
               &result,
               (uint64_t)deadline_ms - 1u,
               &first_token) == PROTO_OK);
    assert(gateway_command_result_validation_release(
        &leases, first_token));
    assert(!gateway_command_result_validation_blocks_timeout(
        &leases, &pending));
    assert(gateway_command_pending_expired(
        &pending, deadline_ms, NULL, NULL));
}

static void test_result_validation_expiry_respects_queried_interval(void)
{
    struct gateway_command_result_validation_leases leases = {0};
    uint32_t token = 0u;

    /* A completed receive is relevant only at its receive timestamp.  Its
     * later lease expiry cannot poison an unrelated command interval. */
    assert(gateway_command_result_validation_arm(
               &leases, 100u, 200u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_complete(
        &leases, token, 150u));
    assert(gateway_command_result_validation_check_interval(
               &leases,
               1000u,
               1100u,
               150u + GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR);

    gateway_command_result_validation_clear(&leases);
    token = 0u;
    assert(gateway_command_result_validation_arm(
               &leases, 100u, 200u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_complete(
        &leases, token, 150u));
    assert(gateway_command_result_validation_check_interval(
               &leases,
               100u,
               200u,
               150u + GATEWAY_COMMAND_RESULT_VALIDATION_MAX_HOLD_MS) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED);

    /* An armed receive can matter only while its possible receive interval
     * overlaps the queried half-open interval [started, deadline). */
    gateway_command_result_validation_clear(&leases);
    token = 0u;
    assert(gateway_command_result_validation_arm(
               &leases, 300u, 400u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_check_interval(
               &leases, 100u, 200u, 400u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR);

    gateway_command_result_validation_clear(&leases);
    token = 0u;
    assert(gateway_command_result_validation_arm(
               &leases, 100u, 200u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_check_interval(
               &leases, 200u, 300u, 200u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_CLEAR);

    gateway_command_result_validation_clear(&leases);
    token = 0u;
    assert(gateway_command_result_validation_arm(
               &leases, 150u, 250u, &token) == PROTO_OK);
    assert(gateway_command_result_validation_check_interval(
               &leases, 200u, 300u, 250u) ==
           GATEWAY_COMMAND_RESULT_VALIDATION_EXPIRED);
}

static void test_collection_preflight_is_exact_and_read_only(void)
{
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
    struct gateway_collection_state collection;
    struct gateway_collection_state before;
    struct proto_packet result;
    struct proto_packet bundle_packet;
    uint8_t result_payload[96];
    uint8_t bundle_payload[256];
    size_t result_payload_len = 0u;
    size_t bundle_payload_len = 0u;
    uint16_t accepted_count = 0u;
    uint16_t duplicate_count = 0u;
    bool duplicate = true;

    id_b.node_id = 0x2222333344445555ull;
    id_b.node_boot_counter++;
    id_b.result_seq++;
    id_c.node_id = 0x3333444455556666ull;
    id_c.node_boot_counter += 2u;
    id_c.result_seq += 2u;
    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    3u,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);

    make_collection_result_payload(result_payload,
                                   sizeof(result_payload),
                                   &result_payload_len,
                                   &id_a,
                                   3003u);
    result = make_collection_result_packet(&id_a, result_payload_len);
    before = collection;
    assert(gateway_collection_preflight_result_from_hop(
               &collection,
               &result,
               result_payload,
               result_payload_len,
               0x777788889999aaaau,
               &duplicate) == PROTO_OK);
    assert(!duplicate);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(gateway_collection_record_result_from_hop(
               &collection,
               &result,
               result_payload,
               result_payload_len,
               0x777788889999aaaau,
               &duplicate) == PROTO_OK);

    before = collection;
    duplicate = false;
    assert(gateway_collection_preflight_result_from_hop(
               &collection,
               &result,
               result_payload,
               result_payload_len,
               0x777788889999aaaau,
               &duplicate) == PROTO_OK);
    assert(duplicate);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);

    make_two_record_collection_bundle(&id_b,
                                      &id_b,
                                      &id_c,
                                      &id_c,
                                      88u,
                                      bundle_payload,
                                      sizeof(bundle_payload),
                                      &bundle_payload_len,
                                      &bundle_packet);
    before = collection;
    assert(gateway_collection_preflight_bundle_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               0x88889999aaaabbbbu,
               &accepted_count,
               &duplicate_count) == PROTO_OK);
    assert(accepted_count == 2u);
    assert(duplicate_count == 0u);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(gateway_collection_record_bundle_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               0x88889999aaaabbbbu,
               &accepted_count,
               &duplicate_count) == PROTO_OK);

    before = collection;
    accepted_count = UINT16_MAX;
    duplicate_count = 0u;
    assert(gateway_collection_preflight_bundle_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               0x88889999aaaabbbbu,
               &accepted_count,
               &duplicate_count) == PROTO_OK);
    assert(accepted_count == 0u);
    assert(duplicate_count == 2u);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
}

static void test_collection_bundle_host_projection_is_canonical(void)
{
    struct command_result_id ids[COLLECTION_BUNDLE_MAX_RECORDS];
    struct command_result_id reversed[2];
    struct gateway_collection_state collection;
    struct gateway_collection_state before;
    struct gateway_collection_bundle_projection projection;
    struct proto_packet direct_packet;
    struct proto_packet bundle_packet;
    struct result_bundle_header projected_header;
    struct result_bundle_record projected_record;
    uint8_t direct_payload[96];
    uint8_t bundle_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    uint8_t projected_payload[PACKET_EXT_MAX_PAYLOAD_LEN];
    size_t direct_payload_len = 0u;
    size_t bundle_payload_len = 0u;
    size_t projected_payload_len = 0u;
    size_t cursor;
    bool duplicate = false;

    ids[0] = (struct command_result_id) {
        .gateway_id = GATEWAY_ID_TEST,
        .gateway_epoch = 9u,
        .command_seq = 1001u,
        .node_id = ANCHOR_ID_TEST,
        .node_boot_counter = 77u,
        .result_seq = 1u,
    };
    for (size_t i = 1u; i < COLLECTION_BUNDLE_MAX_RECORDS; i++) {
        ids[i] = ids[0];
        ids[i].node_id += i;
        ids[i].node_boot_counter += (uint32_t)i;
        ids[i].result_seq += (uint16_t)i;
    }

    assert(gateway_collection_start(&collection,
                                    GATEWAY_ID_TEST,
                                    9u,
                                    1001u,
                                    3003u,
                                    4u,
                                    COLLECTION_BUNDLE_MAX_RECORDS,
                                    0u,
                                    COLLECTION_RETRY_ROUND_0_MS) == PROTO_OK);
    make_collection_result_payload(direct_payload,
                                   sizeof(direct_payload),
                                   &direct_payload_len,
                                   &ids[0],
                                   3003u);
    direct_packet = make_collection_result_packet(&ids[0],
                                                  direct_payload_len);
    assert(gateway_collection_record_result(&collection,
                                            &direct_packet,
                                            direct_payload,
                                            direct_payload_len,
                                            &duplicate) == PROTO_OK);
    assert(!duplicate);

    make_collection_bundle_from_ids(ids,
                                    2u,
                                    90u,
                                    bundle_payload,
                                    sizeof(bundle_payload),
                                    &bundle_payload_len,
                                    &bundle_packet);
    before = collection;
    memset(&projection, 0xa5, sizeof(projection));
    assert(gateway_collection_preflight_bundle_projection_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               bundle_packet.src_id,
               &projection) == PROTO_OK);
    assert(projection.accepted_record_mask == 0x02u);
    assert(projection.accepted_count == 1u);
    assert(projection.duplicate_count == 1u);
    assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    assert(gateway_collection_project_bundle_payload(
               bundle_payload,
               bundle_payload_len,
               projection.accepted_record_mask,
               projected_payload,
               sizeof(projected_payload),
               &projected_payload_len) == PROTO_OK);
    assert(projected_payload_len < bundle_payload_len);
    assert(result_bundle_header_from_tlvs(projected_payload,
                                          projected_payload_len,
                                          &projected_header) == PROTO_OK);
    assert(projected_header.record_count == 1u);
    cursor = first_result_record_offset(projected_payload,
                                        projected_payload_len);
    assert(projected_header.bundle_crc ==
           proto_crc16_ccitt_false(&projected_payload[cursor],
                                   projected_payload_len - cursor));
    assert(result_bundle_record_next_from_tlvs(projected_payload,
                                               projected_payload_len,
                                               &cursor,
                                               &projected_record) == PROTO_OK);
    assert_command_result_id_equal(&projected_record.result_id, &ids[1]);
    assert(cursor == projected_payload_len);

    reversed[0] = ids[1];
    reversed[1] = ids[0];
    make_collection_bundle_from_ids(reversed,
                                    2u,
                                    91u,
                                    bundle_payload,
                                    sizeof(bundle_payload),
                                    &bundle_payload_len,
                                    &bundle_packet);
    assert(gateway_collection_preflight_bundle_projection_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               bundle_packet.src_id,
               &projection) == PROTO_OK);
    assert(projection.accepted_record_mask == 0x01u);
    assert(projection.accepted_count == 1u);
    assert(projection.duplicate_count == 1u);

    reversed[0] = ids[0];
    reversed[1] = ids[0];
    make_collection_bundle_from_ids(reversed,
                                    2u,
                                    92u,
                                    bundle_payload,
                                    sizeof(bundle_payload),
                                    &bundle_payload_len,
                                    &bundle_packet);
    assert(gateway_collection_preflight_bundle_projection_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               bundle_packet.src_id,
               &projection) == PROTO_OK);
    assert(projection.accepted_record_mask == 0u);
    assert(projection.accepted_count == 0u);
    assert(projection.duplicate_count == 2u);
    assert(gateway_collection_project_bundle_payload(
               bundle_payload,
               bundle_payload_len,
               0u,
               projected_payload,
               sizeof(projected_payload),
               &projected_payload_len) == PROTO_ERR_MALFORMED);

    {
        struct command_result_id conflict_payload_id = ids[0];

        conflict_payload_id.result_seq++;
        make_two_record_collection_bundle(&ids[0],
                                          &ids[0],
                                          &ids[1],
                                          &conflict_payload_id,
                                          93u,
                                          bundle_payload,
                                          sizeof(bundle_payload),
                                          &bundle_payload_len,
                                          &bundle_packet);
        memset(&projection, 0xa5, sizeof(projection));
        assert(gateway_collection_preflight_bundle_projection_from_hop(
                   &collection,
                   &bundle_packet,
                   bundle_payload,
                   bundle_payload_len,
                   bundle_packet.src_id,
                   &projection) == PROTO_ERR_MALFORMED);
        assert(projection.accepted_record_mask == 0u);
        assert(projection.accepted_count == 0u);
        assert(projection.duplicate_count == 0u);
        assert(memcmp(&collection, &before, sizeof(collection)) == 0);
    }

    for (size_t i = 2u; i < COLLECTION_BUNDLE_MAX_RECORDS; i += 2u) {
        make_collection_result_payload(direct_payload,
                                       sizeof(direct_payload),
                                       &direct_payload_len,
                                       &ids[i],
                                       3003u);
        direct_packet = make_collection_result_packet(&ids[i],
                                                      direct_payload_len);
        assert(gateway_collection_record_result(&collection,
                                                &direct_packet,
                                                direct_payload,
                                                direct_payload_len,
                                                &duplicate) == PROTO_OK);
        assert(!duplicate);
    }
    make_collection_bundle_from_ids(ids,
                                    COLLECTION_BUNDLE_MAX_RECORDS,
                                    94u,
                                    bundle_payload,
                                    sizeof(bundle_payload),
                                    &bundle_payload_len,
                                    &bundle_packet);
    assert(gateway_collection_preflight_bundle_projection_from_hop(
               &collection,
               &bundle_packet,
               bundle_payload,
               bundle_payload_len,
               bundle_packet.src_id,
               &projection) == PROTO_OK);
    assert(projection.accepted_record_mask == 0xaau);
    assert(projection.accepted_count == 4u);
    assert(projection.duplicate_count == 4u);
    assert(gateway_collection_project_bundle_payload(
               bundle_payload,
               bundle_payload_len,
               projection.accepted_record_mask,
               projected_payload,
               sizeof(projected_payload),
               &projected_payload_len) == PROTO_OK);
    assert(result_bundle_header_from_tlvs(projected_payload,
                                          projected_payload_len,
                                          &projected_header) == PROTO_OK);
    assert(projected_header.record_count == 4u);
    cursor = first_result_record_offset(projected_payload,
                                        projected_payload_len);
    assert(projected_header.bundle_crc ==
           proto_crc16_ccitt_false(&projected_payload[cursor],
                                   projected_payload_len - cursor));
    for (size_t i = 1u; i < COLLECTION_BUNDLE_MAX_RECORDS; i += 2u) {
        assert(result_bundle_record_next_from_tlvs(projected_payload,
                                                   projected_payload_len,
                                                   &cursor,
                                                   &projected_record) == PROTO_OK);
        assert_command_result_id_equal(&projected_record.result_id, &ids[i]);
    }
    assert(cursor == projected_payload_len);

    memcpy(projected_payload, bundle_payload, bundle_payload_len);
    assert(gateway_collection_project_bundle_payload(
               projected_payload,
               bundle_payload_len,
               projection.accepted_record_mask,
               projected_payload,
               sizeof(projected_payload),
               &projected_payload_len) == PROTO_OK);
    assert(result_bundle_header_from_tlvs(projected_payload,
                                          projected_payload_len,
                                          &projected_header) == PROTO_OK);
    assert(projected_header.record_count == 4u);
}

int main(void)
{
    test_prepare_outbound_normalizes_host_command();
    test_prepare_outbound_preserves_host_session_sequence_and_normalizes_ttl();
    test_prepare_outbound_uses_command_class_ttl_for_every_host_value();
    test_prepare_outbound_rejects_invalid_host_packets();
    test_prepare_outbound_rejects_malformed_command_id();
    test_duplicate_command_singletons_are_rejected();
    test_extract_options_defaults_to_single_node_small_result();
    test_extract_options_rejects_unsupported_group_scope();
    test_command_scope_applies_to_explicit_and_derived_membership();
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
    test_command_rx_duplicate_history_blocks_replay_after_delivery_expiry();
    test_command_rx_high_watermark_retains_more_than_legacy_capacity();
    test_command_rx_high_watermark_rejects_uncommitted_older_gap();
    test_command_rx_high_watermark_accepts_wrap_newer_only();
    test_collection_initial_due_is_deterministic_and_bounded();
    test_all_node_collection_due_spreads_responses();
    test_collection_retry_round_advances_spread();
    test_collection_eack_sequence_wraps_nonzero_after_dedup_horizon();
    test_append_collection_result_identity_requires_epoch();
    test_collection_records_unique_results_and_builds_eack();
    test_collection_rejects_crc16_collision();
    test_collection_bundle_rejects_staged_crc16_collision();
    test_collection_prepares_eack_broadcast_outbound();
    test_collection_prepares_missing_list_from_roster();
    test_collection_roster_resolution_binds_explicit_roster_to_membership();
    test_collection_roster_resolution_rejects_provider_mismatch();
    test_collection_roster_resolution_leaves_all_heard_best_effort();
    test_collection_roster_binding_validates_exact_membership();
    test_collection_roster_filters_direct_results();
    test_collection_roster_filters_bundles_atomically();
    test_provider_roster_feeds_missing_list_eack();
    test_collection_records_result_bundle_and_dedupes_replay();
    test_collection_bundle_commits_mixed_new_and_duplicate_results();
    test_collection_bundle_malformed_later_record_rolls_back();
    test_collection_rejects_expected_count_beyond_capacity();
    test_collection_bundle_stale_later_record_rolls_back();
    test_collection_return_candidates_from_direct_results();
    test_collection_bundle_records_return_hop();
    test_collection_return_candidates_suppress_duplicates();
    test_collection_rejects_corrupt_result_bundle();
    test_collection_rejects_wrong_result_identity();
    test_collection_rejects_missing_or_wrong_collection_epoch();
    test_extract_duration_uses_optional_tlv();
    test_command_budget_is_optional_bounded_and_phase_aware();
    test_extract_role_requires_valid_device_role_tlv();
    test_build_failure_result_is_host_visible();
    test_build_success_result_is_not_flagged_as_error();
    test_pending_command_completes_on_matching_result();
    test_pending_command_expires_with_original_context();
    test_pending_command_expiry_handles_ms_wrap();
    test_result_admission_respects_deferred_validation();
    test_pending_command_rejects_second_start();
    test_pending_command_deadline_has_one_terminal_winner();
    test_pending_command_absolute_deadline_is_shared();
    test_result_validation_lease_serializes_deadline();
    test_result_validation_expiry_respects_queried_interval();
    test_collection_preflight_is_exact_and_read_only();
    test_collection_bundle_host_projection_is_canonical();
    return 0;
}
