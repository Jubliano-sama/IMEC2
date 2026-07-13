#include "app_mesh_gateway_command_flow.h"


#include <string.h>

static int find_u8_tlv(const uint8_t *payload,
                       size_t payload_len,
                       uint8_t type,
                       uint8_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(*value)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = tlv_value[0];
    return PROTO_OK;
}

static int gateway_command_flow_find_u16_tlv(const uint8_t *payload,
                                             size_t payload_len,
                                             uint8_t type,
                                             uint16_t *value)
{
    const uint8_t *tlv_value = NULL;
    uint8_t value_len = 0u;
    int ret;

    if (payload == NULL || value == NULL) {
        return PROTO_ERR_ARG;
    }
    ret = tlv_find(payload, payload_len, type, &tlv_value, &value_len);
    if (ret != PROTO_OK) {
        return ret;
    }
    if (value_len != sizeof(*value)) {
        return PROTO_ERR_MALFORMED;
    }
    *value = proto_get_u16_le(tlv_value);
    return PROTO_OK;
}

int app_mesh_gateway_command_flow_prepare(
    struct proto_packet *packet,
    uint8_t *payload,
    size_t payload_len,
    uint64_t gateway_id,
    uint32_t now_ms,
    uint16_t fallback_seq,
    struct app_mesh_gateway_command_flow *flow)
{
    int ret;

    if (packet == NULL || payload == NULL || flow == NULL ||
        packet->msg_type != MSG_COMMAND) {
        return PROTO_ERR_ARG;
    }

    memset(flow, 0, sizeof(*flow));
    ret = gateway_command_prepare_outbound(packet,
                                           payload,
                                           payload_len,
                                           gateway_id,
                                           now_ms,
                                           fallback_seq,
                                           &flow->outbound,
                                           &flow->command_id);
    if (ret != PROTO_OK) {
        return ret;
    }
    ret = gateway_command_extract_options(payload, payload_len, &flow->options);
    if (ret != PROTO_OK) {
        return ret;
    }
    flow->tracking_mode = gateway_command_tracking_mode_from_options(&flow->options);
    return PROTO_OK;
}

int app_mesh_gateway_command_flow_anchor_receive(
    struct app_mesh_gateway_command_anchor_state *state,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t now_ms,
    enum command_id *command_id,
    struct gateway_command_options *options,
    bool *broadcast,
    bool *expired,
    bool *duplicate)
{
    int ret;

    if (packet == NULL || payload == NULL || command_id == NULL ||
        options == NULL || broadcast == NULL || expired == NULL ||
        duplicate == NULL || packet->msg_type != MSG_COMMAND) {
        return PROTO_ERR_ARG;
    }

    memset(options, 0, sizeof(*options));
    *broadcast = packet->dst_id == MESH_BROADCAST_ID;
    *expired = false;
    *duplicate = false;
    ret = gateway_command_extract_id(payload, payload_len, command_id);
    if (ret != PROTO_OK || !*broadcast) {
        return ret;
    }

    ret = gateway_command_extract_options(payload, payload_len, options);
    if (ret != PROTO_OK || !options->flood_required) {
        return ret == PROTO_OK ? PROTO_ERR_MALFORMED : ret;
    }
    *expired = gateway_command_receive_expired(packet, options);
    *duplicate = state != NULL && gateway_command_rx_duplicate_seen(
        &state->duplicate_cache, options->command_seq, now_ms);
    return PROTO_OK;
}

void app_mesh_gateway_command_flow_anchor_remember(
    struct app_mesh_gateway_command_anchor_state *state,
    const struct proto_packet *packet,
    const struct gateway_command_options *options,
    uint32_t now_ms)
{
    if (state != NULL && packet != NULL && options != NULL) {
        gateway_command_rx_duplicate_store(&state->duplicate_cache,
                                           packet,
                                           options,
                                           now_ms);
    }
}

int app_mesh_gateway_command_flow_init_result(
    struct mesh_outbound *outbound,
    const struct proto_packet *command,
    uint64_t source_id,
    uint64_t gateway_id,
    bool diagnostic)
{
    int ret;

    if (outbound == NULL || command == NULL ||
        outbound->payload_len > sizeof(outbound->payload)) {
        return PROTO_ERR_ARG;
    }
    ret = mesh_init_command_result(&outbound->packet,
                                   source_id,
                                   gateway_id,
                                   command->session_id,
                                   command->seq,
                                   (uint8_t)outbound->payload_len,
                                   diagnostic);
    if (ret != PROTO_OK) {
        return ret;
    }
    return PROTO_OK;
}

bool app_mesh_gateway_command_flow_result_matches(
    const struct proto_packet *pending_command,
    const struct proto_packet *result)
{
    return pending_command != NULL && result != NULL &&
           result->msg_type == MSG_COMMAND_RESULT &&
           pending_command->src_id == result->dst_id &&
           pending_command->session_id == result->session_id &&
           pending_command->seq == result->seq;
}

int app_mesh_gateway_command_flow_decode_result(
    enum command_id pending_command_id,
    const uint8_t *payload,
    size_t payload_len,
    enum command_status *status,
    uint8_t *reason)
{
    enum command_id result_command_id = CMD_VENDOR_BASE;
    uint16_t result_command_value;
    uint16_t status_value;
    int ret;

    if (payload == NULL || status == NULL || reason == NULL) {
        return PROTO_ERR_ARG;
    }

    ret = gateway_command_flow_find_u16_tlv(payload, payload_len, TLV_COMMAND_ID,
                                            &result_command_value);
    if (ret == PROTO_OK) {
        ret = gateway_command_flow_find_u16_tlv(payload, payload_len,
                                                TLV_COMMAND_STATUS,
                                                &status_value);
    }
    if (ret == PROTO_OK) {
        ret = find_u8_tlv(payload, payload_len, TLV_REASON, reason);
    }
    if (ret != PROTO_OK) {
        *status = COMMAND_INTERNAL_ERROR;
        *reason = (uint8_t)(-ret);
        return ret;
    }
    if (status_value > COMMAND_INTERNAL_ERROR) {
        *status = COMMAND_INTERNAL_ERROR;
        *reason = 1u;
        return PROTO_ERR_MALFORMED;
    }
    result_command_id = (enum command_id)result_command_value;
    *status = (enum command_status)status_value;
    if (result_command_id != pending_command_id) {
        *status = COMMAND_INTERNAL_ERROR;
        *reason = 1u;
        return PROTO_ERR_MALFORMED;
    }
    return PROTO_OK;
}
