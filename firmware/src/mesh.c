#include "mesh.h"

static bool ids_are_valid(uint64_t src_id, uint64_t dst_id)
{
    return src_id != 0u && dst_id != 0u && src_id != dst_id;
}

static bool command_status_valid(enum command_status status)
{
    return status >= COMMAND_OK && status <= COMMAND_INTERNAL_ERROR;
}

int mesh_append_requested_seq(uint8_t *payload,
                                   size_t payload_cap,
                                   size_t *offset,
                                   uint16_t requested_seq)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_REQUESTED_MSG_SEQ, requested_seq);
}

int mesh_append_command_id(uint8_t *payload,
                                size_t payload_cap,
                                size_t *offset,
                                enum command_id command_id)
{
    return tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_ID, (uint16_t)command_id);
}

int mesh_append_command_result(uint8_t *payload,
                                    size_t payload_cap,
                                    size_t *offset,
                                    enum command_id command_id,
                                    enum command_status status,
                                    uint8_t reason)
{
    int ret;

    if (!command_status_valid(status)) {
        return PROTO_ERR_MALFORMED;
    }

    ret = mesh_append_command_id(payload, payload_cap, offset, command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = tlv_append_u16(payload, payload_cap, offset, TLV_COMMAND_STATUS, (uint16_t)status);
    if (ret != PROTO_OK) {
        return ret;
    }
    return tlv_append_u8(payload, payload_cap, offset, TLV_REASON, reason);
}

int mesh_init_gateway_ack(struct proto_packet *packet,
                               uint64_t gateway_id,
                               uint64_t original_src_id,
                               uint32_t session_id,
                               uint16_t ack_seq,
                               uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, original_src_id) ||
        session_id == 0u ||
        ack_seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_GATEWAY_ACK;
    packet->flags = FLAG_GATEWAY_ACK;
    packet->src_id = gateway_id;
    packet->dst_id = original_src_id;
    packet->session_id = session_id;
    packet->seq = ack_seq;
    packet->ttl = MESH_GATEWAY_ACK_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int mesh_init_command(struct proto_packet *packet,
                           uint64_t gateway_id,
                           uint64_t target_id,
                           uint32_t session_id,
                           uint16_t seq,
                           uint8_t payload_len)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(gateway_id, target_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND;
    packet->flags = 0u;
    packet->src_id = gateway_id;
    packet->dst_id = target_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}

int mesh_init_command_result(struct proto_packet *packet,
                                  uint64_t target_id,
                                  uint64_t gateway_id,
                                  uint32_t session_id,
                                  uint16_t seq,
                                  uint8_t payload_len,
                                  bool diagnostic)
{
    if (packet == NULL) {
        return PROTO_ERR_ARG;
    }
    if (!ids_are_valid(target_id, gateway_id) ||
        session_id == 0u ||
        seq == 0u) {
        return PROTO_ERR_MALFORMED;
    }

    packet->msg_type = MSG_COMMAND_RESULT;
    packet->flags = FLAG_GATEWAY_ACK_REQUIRED;
    if (diagnostic) {
        packet->flags |= FLAG_DIAGNOSTIC;
    }
    packet->src_id = target_id;
    packet->dst_id = gateway_id;
    packet->session_id = session_id;
    packet->seq = seq;
    packet->ttl = MESH_DEFAULT_TTL;
    packet->payload_len = payload_len;
    return PROTO_OK;
}
