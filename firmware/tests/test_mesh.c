#include "mesh.h"

#include <assert.h>

static void test_hop_ack_packet_references_requested_sequence(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet packet = {0};

    assert(mesh_append_requested_seq(payload, sizeof(payload), &payload_len, 77u) == PROTO_OK);
    assert(mesh_init_hop_ack(&packet,
                                  0x1111222233334444ull,
                                  0x5555666677778888ull,
                                  0xAABBCCDDu,
                                  9u,
                                  (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_MESH_ACK);
    assert(packet.flags == FLAG_HOP_ACK);
    assert(packet.src_id == 0x1111222233334444ull);
    assert(packet.dst_id == 0x5555666677778888ull);
    assert(packet.ttl == 1u);
    assert(packet.payload_len == payload_len);

    assert(tlv_find(payload, payload_len, TLV_REQUESTED_MSG_SEQ, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == 77u);
}

static void test_gateway_ack_is_end_to_end(void)
{
    uint8_t payload[16];
    size_t payload_len = 0u;
    struct proto_packet packet = {0};

    assert(mesh_append_requested_seq(payload, sizeof(payload), &payload_len, 101u) == PROTO_OK);
    assert(mesh_init_gateway_ack(&packet,
                                      0x9999888877776666ull,
                                      0x1111222233334444ull,
                                      0x12345678u,
                                      12u,
                                      (uint8_t)payload_len) == PROTO_OK);

    assert(packet.msg_type == MSG_GATEWAY_ACK);
    assert(packet.flags == FLAG_GATEWAY_ACK);
    assert(packet.src_id == 0x9999888877776666ull);
    assert(packet.dst_id == 0x1111222233334444ull);
    assert(packet.ttl == MESH_ACK_TTL);
}

static void test_command_and_result_are_acknowledged_not_clicks(void)
{
    uint8_t payload[32];
    size_t payload_len = 0u;
    const uint8_t *value = NULL;
    uint8_t value_len = 0u;
    struct proto_packet command = {0};
    struct proto_packet result = {0};

    assert(mesh_append_command_id(payload, sizeof(payload), &payload_len, CMD_GET_STATUS) == PROTO_OK);
    assert(mesh_init_command(&command,
                                  0x9999888877776666ull,
                                  0x1111222233334444ull,
                                  0x12345678u,
                                  1u,
                                  (uint8_t)payload_len) == PROTO_OK);

    assert(command.msg_type == MSG_COMMAND);
    assert((command.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((command.flags & FLAG_GATEWAY_ACK_REQUIRED) == 0u);
    assert((command.flags & FLAG_COUNT_AS_CLICK) == 0u);

    payload_len = 0u;
    assert(mesh_append_command_result(payload,
                                           sizeof(payload),
                                           &payload_len,
                                           CMD_GET_STATUS,
                                           COMMAND_OK,
                                           0u) == PROTO_OK);
    assert(mesh_init_command_result(&result,
                                         0x1111222233334444ull,
                                         0x9999888877776666ull,
                                         0x12345678u,
                                         2u,
                                         (uint8_t)payload_len,
                                         true) == PROTO_OK);

    assert(result.msg_type == MSG_COMMAND_RESULT);
    assert((result.flags & FLAG_ACK_REQUESTED) != 0u);
    assert((result.flags & FLAG_GATEWAY_ACK_REQUIRED) != 0u);
    assert((result.flags & FLAG_DIAGNOSTIC) != 0u);
    assert((result.flags & FLAG_COUNT_AS_CLICK) == 0u);

    assert(tlv_find(payload, payload_len, TLV_COMMAND_ID, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == CMD_GET_STATUS);

    assert(tlv_find(payload, payload_len, TLV_COMMAND_STATUS, &value, &value_len) == PROTO_OK);
    assert(value_len == 2u);
    assert(proto_get_u16_le(value) == COMMAND_OK);
}

static void test_rejects_invalid_ids(void)
{
    struct proto_packet packet = {0};

    assert(mesh_init_command(&packet, 0u, 1u, 1u, 1u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_command(&packet, 1u, 1u, 1u, 1u, 0u) == PROTO_ERR_MALFORMED);
    assert(mesh_init_gateway_ack(&packet, 1u, 2u, 0u, 1u, 0u) == PROTO_ERR_MALFORMED);
}

int main(void)
{
    test_hop_ack_packet_references_requested_sequence();
    test_gateway_ack_is_end_to_end();
    test_command_and_result_are_acknowledged_not_clicks();
    test_rejects_invalid_ids();
    return 0;
}
