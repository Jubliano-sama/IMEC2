#include "app_mesh_gateway_ack_policy.h"

#include "protocol.h"

#include <string.h>

static void decision_init(struct app_mesh_gateway_ack_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
}

void app_mesh_gateway_ack_decide(
    const struct app_mesh_gateway_ack_state *state,
    struct app_mesh_gateway_ack_decision *decision)
{
    decision_init(decision);
    if (state == NULL || decision == NULL) {
        return;
    }

    if (state->route_test_enabled &&
        state->gateway_role &&
        state->received_on_channel9 &&
        !state->current_channel9_attempted) {
        decision->action = APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9;
        decision->reason = "gateway-ack-immediate-channel9";
        return;
    }

    if (state->gateway_role &&
        state->received_on_channel9 &&
        !state->current_channel9_attempted) {
        decision->action = APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9;
        decision->reason = "gateway-ack-current-channel9";
        return;
    }

    if (state->current_channel9_attempted && state->current_channel9_ret != 0) {
        decision->action = APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY;
        decision->reason = "gateway-ack-current-channel9";
        decision->delay_ms = state->channel9_retry_delay_ms;
        return;
    }

    if (state->channel9_require_ret == PROTO_OK) {
        decision->action = APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9;
        decision->reason = "gateway-ack";
        return;
    }

    if (state->channel9_require_ret == PROTO_ERR_STALE ||
        state->channel9_require_ret == PROTO_ERR_NOT_FOUND) {
        decision->action = APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9;
        decision->reason = "gateway-ack-channel9-refresh";
        return;
    }

    decision->action = APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY;
    decision->reason = "gateway-ack-channel9-wait";
    decision->delay_ms = state->channel9_retry_delay_ms;
}
