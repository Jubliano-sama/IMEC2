#include "app_mesh_route_ready_handoff.h"

#include <string.h>

static void result_init(struct app_mesh_route_ready_handoff_result *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
}

void app_mesh_route_ready_handoff_on_ready(
    const struct app_mesh_route_ready_handoff_state *state,
    struct app_mesh_route_ready_handoff_result *result)
{
    result_init(result);
    if (result == NULL) {
        return;
    }

    result->clear_route_reply_handoff = true;
    if (state == NULL || !state->selected_route_valid) {
        result->clear_deferred_peer = true;
        result->try_waiting_tx = true;
        return;
    }

    result->peer_id = state->selected_peer_id;
    if (state->selected_timing_valid ||
        state->selected_is_unscheduled_gateway) {
        if (state->deferred_peer_valid &&
            state->deferred_peer_id == state->selected_peer_id) {
            result->clear_deferred_peer = true;
        }
        result->try_waiting_tx = true;
        return;
    }
    if (state->rx_queue_pending) {
        result->remember_deferred_peer = true;
        result->schedule_rx_drain = true;
        return;
    }

    if (state->deferred_peer_valid &&
        state->deferred_peer_id == state->selected_peer_id) {
        result->clear_deferred_peer = true;
    }
    result->propose_now = true;
    result->try_waiting_tx = true;
}

void app_mesh_route_ready_handoff_on_waiting_tx(
    const struct app_mesh_route_ready_handoff_state *state,
    struct app_mesh_route_ready_handoff_result *result)
{
    result_init(result);
    if (result == NULL) {
        return;
    }

    if (state == NULL || !state->deferred_peer_valid) {
        result->allow_waiting_tx = true;
        return;
    }

    result->peer_id = state->deferred_peer_id;
    if (state->rx_queue_pending) {
        result->schedule_propose_wait_rx = true;
        return;
    }

    result->clear_deferred_peer = true;
    result->propose_deferred = true;
    result->allow_waiting_tx = true;
}

void app_mesh_route_ready_handoff_after_proposal(
    int proposal_ret,
    struct app_mesh_route_ready_handoff_result *result)
{
    if (result != NULL && proposal_ret < 0) {
        result->remember_deferred_peer = true;
        result->clear_deferred_peer = false;
        result->schedule_event_accept_wait = true;
        result->try_waiting_tx = false;
        result->allow_waiting_tx = false;
    }
}
