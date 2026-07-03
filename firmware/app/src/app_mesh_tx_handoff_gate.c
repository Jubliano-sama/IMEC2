#include "app_mesh_tx_handoff_gate.h"

#include <string.h>

static enum app_mesh_tx_handoff_reason app_mesh_tx_handoff_select_reason(
    const struct app_mesh_tx_handoff_state *state)
{
    if (state == NULL || !state->queued_gateway_tx_pending) {
        return APP_MESH_TX_HANDOFF_REASON_NONE;
    }
    if (state->rx_control_handoff_active) {
        return APP_MESH_TX_HANDOFF_REASON_RX_CONTROL;
    }
    if (state->route_reply_handoff_active) {
        return APP_MESH_TX_HANDOFF_REASON_ROUTE_REPLY;
    }
    return APP_MESH_TX_HANDOFF_REASON_NONE;
}

bool app_mesh_tx_handoff_gate_yield(
    const struct app_mesh_tx_handoff_state *state,
    const struct app_mesh_tx_handoff_ops *ops,
    struct app_mesh_tx_handoff_result *result)
{
    struct app_mesh_tx_handoff_result local_result;
    enum app_mesh_tx_handoff_reason reason;

    memset(&local_result, 0, sizeof(local_result));
    if (state != NULL) {
        local_result.work = state->work;
    }

    reason = app_mesh_tx_handoff_select_reason(state);
    if (reason == APP_MESH_TX_HANDOFF_REASON_NONE) {
        if (result != NULL) {
            *result = local_result;
        }
        return false;
    }

    local_result.yield = true;
    local_result.keep_queued_tx = true;
    local_result.reason = reason;
    local_result.retry_delay_ms = state->retry_delay_ms;

    if (ops != NULL && ops->schedule_retry != NULL) {
        local_result.schedule_ret = ops->schedule_retry(state->work,
                                                        reason,
                                                        state->retry_delay_ms,
                                                        ops->ctx);
        local_result.retry_scheduled = local_result.schedule_ret >= 0;
    }

    if (result != NULL) {
        *result = local_result;
    }
    return true;
}
