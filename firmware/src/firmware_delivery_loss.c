#include "firmware_delivery_loss.h"

#include "mesh_relay.h"
#include "protocol.h"

#include <limits.h>
#include <string.h>

void fw_delivery_loss_init(struct fw_delivery_loss_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static bool same_delivery(const struct mesh_outbound *lhs,
                          const struct mesh_outbound *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->packet.msg_type == rhs->packet.msg_type &&
           lhs->packet.src_id == rhs->packet.src_id &&
           lhs->packet.dst_id == rhs->packet.dst_id &&
           lhs->packet.session_id == rhs->packet.session_id &&
           lhs->packet.seq == rhs->packet.seq;
}

static void increment_loss(struct fw_delivery_loss_state *state)
{
    if (state != NULL && state->lost_count < UINT32_MAX) {
        state->lost_count++;
    }
}

void fw_delivery_loss_note_store(
    struct fw_delivery_loss_state *state,
    bool existing_valid,
    const struct mesh_outbound *existing,
    const struct mesh_outbound *replacement,
    struct fw_delivery_loss_store_result *result)
{
    struct fw_delivery_loss_store_result local = {0};

    if (state != NULL && replacement != NULL) {
        if (existing_valid && !same_delivery(existing, replacement)) {
            increment_loss(state);
            local.replaced_existing = true;
        }
        local.lost_count = state->lost_count;
    }
    if (result != NULL) {
        *result = local;
    }
}

void fw_delivery_loss_note_drop(
    struct fw_delivery_loss_state *state,
    struct fw_delivery_loss_store_result *result)
{
    struct fw_delivery_loss_store_result local = {0};

    if (state != NULL) {
        increment_loss(state);
        local.replaced_existing = true;
        local.lost_count = state->lost_count;
    }
    if (result != NULL) {
        *result = local;
    }
}

static int update_loss_tlv(struct mesh_outbound *out,
                           uint32_t lost_count,
                           bool *updated)
{
    size_t offset = 0u;

    if (updated != NULL) {
        *updated = false;
    }
    if (out == NULL) {
        return PROTO_ERR_ARG;
    }
    while (offset < out->payload_len) {
        uint8_t type;
        uint8_t len;

        if ((size_t)out->payload_len - offset < 2u) {
            return PROTO_ERR_MALFORMED;
        }
        type = out->payload[offset];
        len = out->payload[offset + 1u];
        if ((size_t)out->payload_len - offset - 2u < len) {
            return PROTO_ERR_MALFORMED;
        }
        if (type == TLV_MESH_LOST_PACKET_COUNT) {
            if (len != 4u) {
                return PROTO_ERR_MALFORMED;
            }
            proto_put_u32_le(&out->payload[offset + 2u], lost_count);
            if (updated != NULL) {
                *updated = true;
            }
            return PROTO_OK;
        }
        offset += 2u + len;
    }
    return PROTO_ERR_NOT_FOUND;
}

int fw_delivery_loss_attach(
    const struct fw_delivery_loss_state *state,
    struct mesh_outbound *out,
    struct fw_delivery_loss_attach_result *result)
{
    struct fw_delivery_loss_attach_result local = {0};
    bool updated = false;
    size_t payload_len;
    int ret;

    if (state == NULL || out == NULL) {
        ret = PROTO_ERR_ARG;
    } else if (state->lost_count == 0u) {
        ret = PROTO_OK;
    } else {
        local.lost_count = state->lost_count;
        local.lost_count_pending = true;
        ret = update_loss_tlv(out, state->lost_count, &updated);
        if (ret == PROTO_ERR_NOT_FOUND) {
            payload_len = out->payload_len;
            ret = tlv_append_u32(out->payload, sizeof(out->payload),
                                 &payload_len,
                                 TLV_MESH_LOST_PACKET_COUNT,
                                 state->lost_count);
            if (ret == PROTO_OK) {
                out->payload_len = (uint16_t)payload_len;
                out->packet.payload_len = (uint16_t)payload_len;
                local.tlv_attached = true;
            }
        } else if (ret == PROTO_OK && updated) {
            local.tlv_updated = true;
        }
    }
    local.ret = ret;
    if (result != NULL) {
        *result = local;
    }
    return ret;
}

void fw_delivery_loss_note_sent(struct fw_delivery_loss_state *state,
                                const struct mesh_outbound *sent)
{
    const uint8_t *value;
    uint8_t len;

    if (state == NULL || sent == NULL || state->lost_count == 0u) {
        return;
    }
    if (tlv_find_unique(sent->payload, sent->payload_len,
                        TLV_MESH_LOST_PACKET_COUNT,
                        &value, &len) == PROTO_OK && len == 4u) {
        state->lost_count = 0u;
    }
}

uint32_t fw_delivery_loss_count(const struct fw_delivery_loss_state *state)
{
    return state == NULL ? 0u : state->lost_count;
}
