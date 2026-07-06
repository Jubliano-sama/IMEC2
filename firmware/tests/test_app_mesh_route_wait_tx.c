#include "app_mesh_route_wait_tx.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#define CHANNEL9_RETRY_MS 50u
#define BUSY_RETRY_MS 1000u

static struct app_mesh_route_wait_tx_state base_state(void)
{
    const struct app_mesh_route_wait_tx_state state = {
        .outbound_ready = true,
        .channel9_retry_delay_ms = CHANNEL9_RETRY_MS,
        .busy_retry_delay_ms = BUSY_RETRY_MS,
    };

    return state;
}

static void test_not_ready_schedules_route_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.outbound_ready = false;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_ROUTE_RETRY);
    assert(strcmp(decision.reason, "route-waiting-not-ready") == 0);
}

static void test_success_clears_waiting_packet(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = 0;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_CLEAR_VALID);
}

static void test_unreachable_requests_route_once(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_REQUEST_ROUTE);
    assert(strcmp(decision.reason, "route-waiting-packet") == 0);
}

static void test_unreachable_route_request_timeout_schedules_slow_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    state.route_request_attempted = true;
    state.route_request_ret = -ETIMEDOUT;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action ==
           APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-exhausted") == 0);
}

static void test_unreachable_successful_route_request_waits(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EHOSTUNREACH;
    state.route_request_attempted = true;
    state.route_request_ret = 0;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_NONE);
}

static void test_tx_timeout_schedules_slow_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -ETIMEDOUT;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action ==
           APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_EXHAUSTED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-stale") == 0);
}

static void test_busy_schedules_channel9_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EBUSY;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-channel9-event") == 0);
    assert(decision.delay_ms == CHANNEL9_RETRY_MS);
}

static void test_other_failure_schedules_busy_retry(void)
{
    struct app_mesh_route_wait_tx_state state = base_state();
    struct app_mesh_route_wait_tx_decision decision;

    state.tx_ret = -EINVAL;
    app_mesh_route_wait_tx_decide(&state, &decision);

    assert(decision.action == APP_MESH_ROUTE_WAIT_TX_ACTION_SCHEDULE_FIXED_RETRY);
    assert(strcmp(decision.reason, "route-waiting-busy") == 0);
    assert(decision.delay_ms == BUSY_RETRY_MS);
}

int main(void)
{
    test_not_ready_schedules_route_retry();
    test_success_clears_waiting_packet();
    test_unreachable_requests_route_once();
    test_unreachable_route_request_timeout_schedules_slow_retry();
    test_unreachable_successful_route_request_waits();
    test_tx_timeout_schedules_slow_retry();
    test_busy_schedules_channel9_retry();
    test_other_failure_schedules_busy_retry();
    return 0;
}
