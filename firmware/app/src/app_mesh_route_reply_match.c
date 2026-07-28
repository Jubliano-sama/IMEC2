#include "app_mesh_route_reply_match.h"

#include "mesh_relay.h"
#include "protocol.h"

#include <string.h>

static bool same_tlv_value(const uint8_t *lhs,
                           size_t lhs_len,
                           const uint8_t *rhs,
                           size_t rhs_len,
                           uint8_t type)
{
    const uint8_t *lhs_value = NULL;
    const uint8_t *rhs_value = NULL;
    uint8_t lhs_value_len = 0u;
    uint8_t rhs_value_len = 0u;

    if (tlv_find(lhs, lhs_len, type, &lhs_value, &lhs_value_len) != PROTO_OK ||
        tlv_find(rhs, rhs_len, type, &rhs_value, &rhs_value_len) != PROTO_OK ||
        lhs_value_len != rhs_value_len) {
        return false;
    }
    return memcmp(lhs_value, rhs_value, lhs_value_len) == 0;
}

bool app_mesh_route_reply_ack_matches(
    const struct mesh_outbound *route_reply,
    const struct proto_packet *packet,
    const uint8_t *payload,
    size_t payload_len,
    uint64_t previous_hop_id,
    uint64_t local_id)
{
    if (route_reply == NULL || packet == NULL || local_id == 0u ||
        (payload == NULL && payload_len != 0u) ||
        packet->msg_type != MSG_ROUTE_REPLY_ACK ||
        packet->src_id != route_reply->next_hop_id ||
        packet->dst_id != local_id ||
        previous_hop_id != route_reply->next_hop_id ||
        packet->session_id != route_reply->packet.session_id) {
        return false;
    }

    return same_tlv_value(route_reply->payload, route_reply->payload_len,
                          payload, payload_len, TLV_INITIATOR_ID) &&
           same_tlv_value(route_reply->payload, route_reply->payload_len,
                          payload, payload_len, TLV_RESPONDER_ID) &&
           same_tlv_value(route_reply->payload, route_reply->payload_len,
                          payload, payload_len, TLV_FLOOD_EPOCH_ID) &&
           same_tlv_value(route_reply->payload, route_reply->payload_len,
                          payload, payload_len, TLV_REPLY_NONCE) &&
           same_tlv_value(route_reply->payload, route_reply->payload_len,
                          payload, payload_len, TLV_METRIC_CRC);
}
