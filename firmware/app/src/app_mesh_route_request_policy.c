#include "app_mesh_route_request_policy.h"

#include "protocol.h"

#include <string.h>

static void decision_init(struct app_mesh_route_request_policy_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
}

void app_mesh_route_request_policy_decide(
    const struct app_mesh_route_request_policy_state *state,
    struct app_mesh_route_request_policy_decision *decision)
{
    decision_init(decision);
    if (state == NULL || decision == NULL) {
        return;
    }

    decision->route_request_flags = state->relay_required ?
                                    MESH_ROUTE_REQ_FLAG_RELAY_REQUIRED : 0u;
    decision->install_direct_route_from_probe =
        !state->relay_required && !state->direct_bulk_suppressed;
    decision->direct_probe_satisfies_request =
        state->direct_probe_ret == 0 &&
        decision->install_direct_route_from_probe;
}

void app_mesh_route_request_rf_failure_decide(
    enum app_mesh_route_request_rf_phase phase,
    int ret,
    bool embedded_route_sent,
    int radio_busy_ret,
    struct app_mesh_route_request_rf_failure_decision *decision)
{
    if (decision != NULL) {
        memset(decision, 0, sizeof(*decision));
    }
    if (decision == NULL || ret != radio_busy_ret) {
        return;
    }

    switch (phase) {
    case APP_MESH_ROUTE_REQUEST_RF_WAKE_TRAIN:
        decision->defer_retry = true;
        decision->restore_prepared_attempt = true;
        break;
    case APP_MESH_ROUTE_REQUEST_RF_CONTROL_TX:
        decision->defer_retry = true;
        decision->restore_prepared_attempt = !embedded_route_sent;
        break;
    default:
        break;
    }
}
