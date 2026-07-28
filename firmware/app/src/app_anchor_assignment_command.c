#include "app_anchor_assignment_command.h"

#include "gateway_command.h"
#include "mesh_relay.h"
#include "operation_policy.h"
#include "protocol.h"

#include <string.h>

int app_anchor_assignment_command_prepare(
    struct mesh_outbound *outbound,
    const struct app_anchor_assignment_command_params *params)
{
    size_t payload_len = 0u;
    int ret;

    if (outbound == NULL || params == NULL ||
        params->epoch == 0u || params->command_seq == 0u) {
        return PROTO_ERR_ARG;
    }

    memset(outbound, 0, sizeof(*outbound));
    ret = tlv_append_u16(outbound->payload,
                         sizeof(outbound->payload),
                         &payload_len,
                         TLV_COMMAND_ID,
                         CMD_ASSIGN_DISCOVERY_SLOTS);
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_SCOPE,
                            CMD_SCOPE_ALL_HEARD);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u8(outbound->payload,
                            sizeof(outbound->payload),
                            &payload_len,
                            TLV_COMMAND_RESPONSE_MODE,
                            CMD_RESPONSE_NONE);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_SEQ,
                             params->command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_FLOOD_EPOCH_ID,
                             params->command_seq);
    }
    if (ret == PROTO_OK) {
        ret = tlv_append_u32(outbound->payload,
                             sizeof(outbound->payload),
                             &payload_len,
                             TLV_COMMAND_EXPIRY_S,
                             params->command_expiry_s);
    }
    if (ret == PROTO_OK) {
        ret = discovery_assignment_append_control_tlvs(
            outbound->payload,
            sizeof(outbound->payload),
            &payload_len,
            params->phase,
            params->epoch);
    }
    if (ret == PROTO_OK) {
        const struct operation_policy policy = {
            .family = OPERATION_POLICY_FAMILY_ASSIGNMENT,
            .value.assignment = {
                .expected_anchor_count = params->expected_anchor_count,
                .operation_budget_ms = params->operation_budget_ms,
                .response_spread_ms = params->response_spread_ms,
            },
        };

        ret = operation_policy_append_tlv(outbound->payload,
                                          sizeof(outbound->payload),
                                          &payload_len,
                                          &policy);
    }
    if (ret != PROTO_OK) {
        return ret;
    }

    outbound->packet.msg_type = MSG_COMMAND;
    outbound->packet.flags = FLAG_DIAGNOSTIC;
    outbound->packet.src_id = params->source_id;
    outbound->packet.dst_id = MESH_BROADCAST_ID;
    outbound->packet.session_id = params->command_seq;
    outbound->packet.ttl = params->ttl;
    outbound->packet.payload_len = (uint16_t)payload_len;
    outbound->payload_len = (uint16_t)payload_len;
    outbound->next_hop_id = MESH_BROADCAST_ID;
    outbound->radio_channel = params->radio_channel;
    return PROTO_OK;
}
