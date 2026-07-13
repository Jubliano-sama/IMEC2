#include "app_mesh_coordinator_runtime.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

void app_mesh_coordinator_runtime_reset(
    struct app_mesh_coordinator_runtime_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

int app_mesh_coordinator_runtime_capture_inputs(
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_inputs *inputs)
{
    if (capture == NULL || inputs == NULL) {
        return -EINVAL;
    }

    *inputs = (struct app_mesh_coordinator_inputs){
        .click_priority = capture->click_active,
        .survey_pending = capture->survey_pending,
        .rx_queue_pending = capture->rx_queue_used > 0u,
        .relay_tx_active = capture->relay_tx_active,
        .route_waiting_tx_active = capture->route_waiting_tx_active,
        .ch9_ack_wait_active = capture->ch9_ack_wait_active,
        .ch9_ack_send_pending = capture->ch9_ack_send_pending,
        .report_queue_pending = capture->report_queue_used > 0u,
        .gateway_continuous_ch9 = capture->gateway_continuous_ch9,
    };
    return 0;
}

int app_mesh_coordinator_runtime_decide(
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_runtime_state *state,
    struct app_mesh_coordinator_decision *decision,
    bool *state_changed)
{
    struct app_mesh_coordinator_inputs inputs;

    if (state_changed != NULL) {
        *state_changed = false;
    }

    if (app_mesh_coordinator_runtime_capture_inputs(capture, &inputs) < 0 ||
        decision == NULL) {
        return -EINVAL;
    }

    app_mesh_coordinator_decide(&inputs, decision);
    if (state == NULL) {
        return 0;
    }

    if (!state->last_state_valid || state->last_state != decision->state) {
        state->last_state = decision->state;
        state->last_state_valid = true;
        if (state_changed != NULL) {
            *state_changed = true;
        }
    }
    return 0;
}
