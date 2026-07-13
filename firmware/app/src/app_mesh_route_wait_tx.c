#include "app_mesh_route_wait_tx.h"

#include <errno.h>
#include <string.h>

static void decision_init(struct app_mesh_route_wait_tx_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
}

bool app_mesh_route_wait_tx_may_store(
    enum app_mesh_route_wait_tx_owner owner)
{
    return owner == APP_MESH_ROUTE_WAIT_TX_OWNER_GENERIC;
}

void app_mesh_route_retry_identity_select(
    enum app_mesh_route_wait_tx_owner owner,
    const struct proto_packet *packet,
    uint32_t generation,
    struct app_mesh_route_retry_identity *identity)
{
    if (identity == NULL) {
        return;
    }
    identity->mode = APP_MESH_DIRECT_GATEWAY_RETRY_ROUTE;
    identity->survey_id = 0u;
    if (packet == NULL ||
        packet->msg_type != MSG_SURVEY_DISCOVERY_REPORT ||
        packet->src_id == 0u || packet->session_id == 0u) {
        return;
    }
    if (owner == APP_MESH_ROUTE_WAIT_TX_OWNER_DURABLE_LOCAL &&
        (generation == 0u || generation != packet->session_id)) {
        return;
    }
    identity->mode = APP_MESH_DIRECT_GATEWAY_RETRY_SURVEY;
    identity->survey_id = packet->session_id;
}

void app_mesh_route_wait_tx_decide(
    const struct app_mesh_route_wait_tx_state *state,
    struct app_mesh_route_wait_tx_decision *decision)
{
    decision_init(decision);
    if (decision == NULL || state == NULL) {
        return;
    }

    if (!state->outbound_ready) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY;
        decision->reason = "route-waiting-not-ready";
        return;
    }

    if (state->tx_ret == 0) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID;
        return;
    }

    if (state->tx_ret == -EHOSTUNREACH) {
        if (!state->route_request_attempted) {
            decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE;
            decision->reason = "route-waiting-packet";
            return;
        }
        if (state->route_request_ret == -ETIMEDOUT) {
            decision->action =
                APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY;
            decision->reason = "route-waiting-exhausted";
        }
        return;
    }

    if (state->tx_ret == -ETIMEDOUT) {
        decision->action =
            APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY;
        decision->reason = "route-waiting-stale";
        return;
    }

    if (state->tx_ret == -EBUSY) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY;
        decision->reason = "route-waiting-channel9-event";
        decision->delay_ms = state->channel9_retry_delay_ms;
        return;
    }

    decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY;
    decision->reason = "route-waiting-busy";
    decision->delay_ms = state->busy_retry_delay_ms;
}
