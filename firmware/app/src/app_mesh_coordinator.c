#include "app_mesh_coordinator.h"

#include "protocol.h"

#include <string.h>

static void decision_init(struct app_mesh_coordinator_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
        decision->state = APP_MESH_COORDINATOR_IDLE;
        decision->mesh_work_allowed = true;
        decision->route_wait_allowed = true;
        decision->report_tx_allowed = true;
        decision->uwb_rx_allowed = true;
        decision->reason = "idle";
    }
}

void app_mesh_coordinator_decide(
    const struct app_mesh_coordinator_inputs *inputs,
    struct app_mesh_coordinator_decision *decision)
{
    decision_init(decision);
    if (inputs == NULL || decision == NULL) {
        return;
    }

    if (inputs->click_priority) {
        decision->state = APP_MESH_COORDINATOR_CLICK;
        decision->mesh_work_allowed = false;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "click";
        return;
    }

    if (inputs->survey_pending) {
        decision->state = APP_MESH_COORDINATOR_SURVEY;
        decision->mesh_work_allowed = false;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "survey";
        return;
    }

    if (inputs->rx_queue_pending) {
        decision->state = APP_MESH_COORDINATOR_MESH_RX;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = "mesh-rx";
        return;
    }

    if (inputs->ch9_ack_send_pending) {
        decision->state = APP_MESH_COORDINATOR_MESH_RX;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = "ch9-ack-send";
        return;
    }

    if (inputs->ch9_ack_wait_active) {
        decision->state = APP_MESH_COORDINATOR_MESH_RX;
        decision->route_wait_allowed = false;
        decision->report_tx_allowed = false;
        decision->reason = "ch9-ack-wait";
        return;
    }

    if (inputs->relay_tx_active ||
        inputs->route_waiting_tx_active) {
        decision->state = APP_MESH_COORDINATOR_MESH_TX;
        decision->report_tx_allowed = false;
        decision->uwb_rx_allowed = false;
        decision->reason = "mesh-tx";
        return;
    }

    if (inputs->report_queue_pending) {
        decision->state = APP_MESH_COORDINATOR_MESH_TX;
        decision->uwb_rx_allowed = false;
        decision->reason = "mesh-tx";
        return;
    }

    if (inputs->gateway_continuous_ch9) {
        decision->state = APP_MESH_COORDINATOR_GATEWAY_RX;
        decision->reason = "gateway-rx";
    }
}

const char *app_mesh_coordinator_state_name(enum app_mesh_coordinator_state state)
{
    switch (state) {
    case APP_MESH_COORDINATOR_IDLE:
        return "idle";
    case APP_MESH_COORDINATOR_CLICK:
        return "click";
    case APP_MESH_COORDINATOR_SURVEY:
        return "survey";
    case APP_MESH_COORDINATOR_MESH_RX:
        return "mesh-rx";
    case APP_MESH_COORDINATOR_MESH_TX:
        return "mesh-tx";
    case APP_MESH_COORDINATOR_GATEWAY_RX:
        return "gateway-rx";
    default:
        return "unknown";
    }
}

void app_mesh_paused_delivery_reset(struct app_mesh_paused_delivery_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

static bool outbound_same_delivery(const struct mesh_outbound *lhs,
                                   const struct mesh_outbound *rhs)
{
    return lhs != NULL &&
           rhs != NULL &&
           lhs->packet.msg_type == rhs->packet.msg_type &&
           lhs->packet.src_id == rhs->packet.src_id &&
           lhs->packet.dst_id == rhs->packet.dst_id &&
           lhs->packet.session_id == rhs->packet.session_id &&
           lhs->packet.seq == rhs->packet.seq;
}

static void lost_count_increment(struct app_mesh_paused_delivery_state *state)
{
    if (state == NULL) {
        return;
    }
    if (state->lost_count < UINT32_MAX) {
        state->lost_count++;
    }
}

void app_mesh_paused_delivery_note_store(
    struct app_mesh_paused_delivery_state *state,
    bool existing_valid,
    const struct mesh_outbound *existing,
    const struct mesh_outbound *replacement,
    struct app_mesh_paused_delivery_store_result *result)
{
    struct app_mesh_paused_delivery_store_result local_result = {0};

    if (state == NULL || replacement == NULL) {
        if (result != NULL) {
            *result = local_result;
        }
        return;
    }

    if (existing_valid && !outbound_same_delivery(existing, replacement)) {
        lost_count_increment(state);
        local_result.replaced_existing = true;
    }
    local_result.lost_count = state->lost_count;
    if (result != NULL) {
        *result = local_result;
    }
}

void app_mesh_paused_delivery_note_drop(
    struct app_mesh_paused_delivery_state *state,
    struct app_mesh_paused_delivery_store_result *result)
{
    struct app_mesh_paused_delivery_store_result local_result = {0};

    if (state != NULL) {
        lost_count_increment(state);
        local_result.replaced_existing = true;
        local_result.lost_count = state->lost_count;
    }
    if (result != NULL) {
        *result = local_result;
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

int app_mesh_paused_delivery_attach_loss(
    const struct app_mesh_paused_delivery_state *state,
    struct mesh_outbound *out,
    struct app_mesh_paused_delivery_attach_result *result)
{
    struct app_mesh_paused_delivery_attach_result local_result = {0};
    bool updated = false;
    size_t payload_len;
    int ret;

    if (state == NULL || out == NULL) {
        ret = PROTO_ERR_ARG;
        local_result.ret = ret;
        if (result != NULL) {
            *result = local_result;
        }
        return ret;
    }

    local_result.lost_count = state->lost_count;
    local_result.lost_count_pending = state->lost_count != 0u;
    if (state->lost_count == 0u) {
        local_result.ret = PROTO_OK;
        if (result != NULL) {
            *result = local_result;
        }
        return PROTO_OK;
    }

    ret = update_loss_tlv(out, state->lost_count, &updated);
    if (ret == PROTO_ERR_NOT_FOUND) {
        payload_len = out->payload_len;
        ret = tlv_append_u32(out->payload,
                             sizeof(out->payload),
                             &payload_len,
                             TLV_MESH_LOST_PACKET_COUNT,
                             state->lost_count);
        if (ret == PROTO_OK) {
            out->payload_len = (uint16_t)payload_len;
            out->packet.payload_len = (uint16_t)payload_len;
            local_result.tlv_attached = true;
        }
    } else if (ret == PROTO_OK && updated) {
        local_result.tlv_updated = true;
    }

    local_result.ret = ret;
    if (result != NULL) {
        *result = local_result;
    }
    return ret;
}

void app_mesh_paused_delivery_note_sent(
    struct app_mesh_paused_delivery_state *state,
    const struct mesh_outbound *sent)
{
    const uint8_t *value;
    uint8_t len;

    if (state == NULL || sent == NULL || state->lost_count == 0u) {
        return;
    }

    if (tlv_find(sent->payload,
                 sent->payload_len,
                 TLV_MESH_LOST_PACKET_COUNT,
                 &value,
                 &len) == PROTO_OK &&
        len == 4u) {
        state->lost_count = 0u;
    }
}

uint32_t app_mesh_paused_delivery_lost_count(
    const struct app_mesh_paused_delivery_state *state)
{
    return state == NULL ? 0u : state->lost_count;
}
