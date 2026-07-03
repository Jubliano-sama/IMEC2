#include "app_mesh_route_wait_tx.h"

#include <errno.h>
#include <string.h>

static void decision_init(struct app_mesh_route_wait_tx_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
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
            decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_DROP;
            decision->reason = "route-waiting-timeout";
        }
        return;
    }

    if (state->tx_ret == -ETIMEDOUT) {
        decision->action = APP_MESH_ROUTE_WAIT_TX_ACTION_DROP;
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
