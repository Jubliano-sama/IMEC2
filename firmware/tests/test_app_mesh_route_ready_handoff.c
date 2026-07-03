#include "app_mesh_route_ready_handoff.h"

#include <assert.h>

static void test_rx_pending_with_selected_route_defers_proposal(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .selected_route_valid = true,
        .rx_queue_pending = true,
        .selected_peer_id = 0x1002u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_ready(&state, &result);

    assert(result.clear_route_reply_handoff);
    assert(result.remember_deferred_peer);
    assert(result.schedule_rx_drain);
    assert(!result.propose_now);
    assert(!result.try_waiting_tx);
    assert(result.peer_id == 0x1002u);
}

static void test_route_ready_without_rx_pending_proposes_and_allows_tx(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .selected_route_valid = true,
        .selected_peer_id = 0x2003u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_ready(&state, &result);

    assert(result.clear_route_reply_handoff);
    assert(result.propose_now);
    assert(result.try_waiting_tx);
    assert(!result.schedule_rx_drain);
    assert(result.peer_id == 0x2003u);
}

static void test_route_ready_success_clears_matching_deferred_peer(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .selected_route_valid = true,
        .deferred_peer_valid = true,
        .selected_peer_id = 0x3004u,
        .deferred_peer_id = 0x3004u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_ready(&state, &result);

    assert(result.propose_now);
    assert(result.clear_deferred_peer);
}

static void test_proposal_failure_requests_event_accept_wait(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .selected_route_valid = true,
        .selected_peer_id = 0x4005u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_ready(&state, &result);
    app_mesh_route_ready_handoff_after_proposal(-1, &result);

    assert(result.propose_now);
    assert(result.schedule_event_accept_wait);
    assert(!result.try_waiting_tx);
}

static void test_waiting_tx_waits_for_rx_to_drain_before_deferred_proposal(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .rx_queue_pending = true,
        .deferred_peer_valid = true,
        .deferred_peer_id = 0x5006u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_waiting_tx(&state, &result);

    assert(result.schedule_propose_wait_rx);
    assert(!result.propose_deferred);
    assert(!result.allow_waiting_tx);
    assert(result.peer_id == 0x5006u);
}

static void test_rx_drained_with_deferred_peer_proposes_once(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .deferred_peer_valid = true,
        .deferred_peer_id = 0x6007u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_waiting_tx(&state, &result);

    assert(result.propose_deferred);
    assert(result.clear_deferred_peer);
    assert(result.allow_waiting_tx);
    assert(result.peer_id == 0x6007u);
}

static void test_deferred_proposal_failure_requests_event_accept_wait(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .deferred_peer_valid = true,
        .deferred_peer_id = 0x7008u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_waiting_tx(&state, &result);
    app_mesh_route_ready_handoff_after_proposal(-5, &result);

    assert(result.propose_deferred);
    assert(result.schedule_event_accept_wait);
    assert(!result.allow_waiting_tx);
}

static void test_no_selected_route_only_clears_handoff_and_tries_tx(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .rx_queue_pending = true,
        .deferred_peer_valid = true,
        .deferred_peer_id = 0x8009u,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_ready(&state, &result);

    assert(result.clear_route_reply_handoff);
    assert(result.clear_deferred_peer);
    assert(result.try_waiting_tx);
    assert(!result.remember_deferred_peer);
    assert(!result.schedule_rx_drain);
    assert(!result.propose_now);
}

static void test_waiting_tx_without_deferred_peer_is_allowed(void)
{
    const struct app_mesh_route_ready_handoff_state state = {
        .rx_queue_pending = false,
    };
    struct app_mesh_route_ready_handoff_result result;

    app_mesh_route_ready_handoff_on_waiting_tx(&state, &result);

    assert(result.allow_waiting_tx);
    assert(!result.propose_deferred);
    assert(!result.schedule_propose_wait_rx);
}

int main(void)
{
    test_rx_pending_with_selected_route_defers_proposal();
    test_route_ready_without_rx_pending_proposes_and_allows_tx();
    test_route_ready_success_clears_matching_deferred_peer();
    test_proposal_failure_requests_event_accept_wait();
    test_waiting_tx_waits_for_rx_to_drain_before_deferred_proposal();
    test_rx_drained_with_deferred_peer_proposes_once();
    test_deferred_proposal_failure_requests_event_accept_wait();
    test_no_selected_route_only_clears_handoff_and_tries_tx();
    test_waiting_tx_without_deferred_peer_is_allowed();
    return 0;
}
