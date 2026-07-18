#ifndef APP_MESH_ROUTE_READY_HANDOFF_H
#define APP_MESH_ROUTE_READY_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

struct app_mesh_route_ready_handoff_state {
    bool selected_route_valid;
    bool selected_timing_valid;
    bool selected_is_unscheduled_gateway;
    bool rx_queue_pending;
    bool deferred_peer_valid;
    uint64_t selected_peer_id;
    uint64_t deferred_peer_id;
};

struct app_mesh_route_ready_handoff_result {
    bool clear_route_reply_handoff;
    bool remember_deferred_peer;
    bool clear_deferred_peer;
    bool propose_now;
    bool propose_deferred;
    bool schedule_rx_drain;
    bool schedule_propose_wait_rx;
    bool schedule_event_accept_wait;
    bool try_waiting_tx;
    bool allow_waiting_tx;
    uint64_t peer_id;
};

void app_mesh_route_ready_handoff_on_ready(
    const struct app_mesh_route_ready_handoff_state *state,
    struct app_mesh_route_ready_handoff_result *result);

void app_mesh_route_ready_handoff_on_waiting_tx(
    const struct app_mesh_route_ready_handoff_state *state,
    struct app_mesh_route_ready_handoff_result *result);

void app_mesh_route_ready_handoff_after_proposal(
    int proposal_ret,
    struct app_mesh_route_ready_handoff_result *result);

#endif
