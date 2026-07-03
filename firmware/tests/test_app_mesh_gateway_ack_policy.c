#include "app_mesh_gateway_ack_policy.h"

#include "protocol.h"

#include <assert.h>
#include <string.h>

static void decide(const struct app_mesh_gateway_ack_state *state,
                   struct app_mesh_gateway_ack_decision *decision)
{
    memset(decision, 0xaa, sizeof(*decision));
    app_mesh_gateway_ack_decide(state, decision);
}

static void test_route_test_channel9_acks_are_batched(void)
{
    const struct app_mesh_gateway_ack_state state = {
        .route_test_enabled = true,
        .gateway_role = true,
        .received_on_channel9 = true,
        .channel9_require_ret = PROTO_OK,
        .channel9_retry_delay_ms = 25u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);

    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_QUEUE_ROUTE_TEST_ACK);
    assert(strcmp(decision.reason, "gateway-ack-route-test-batch") == 0);
}

static void test_gateway_rx_on_channel9_tries_current_event_first(void)
{
    const struct app_mesh_gateway_ack_state state = {
        .gateway_role = true,
        .received_on_channel9 = true,
        .channel9_require_ret = PROTO_OK,
        .channel9_retry_delay_ms = 25u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);

    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_CURRENT_CHANNEL9);
    assert(strcmp(decision.reason, "gateway-ack-current-channel9") == 0);
}

static void test_current_channel9_failure_waits_for_channel9_retry(void)
{
    const struct app_mesh_gateway_ack_state state = {
        .gateway_role = true,
        .received_on_channel9 = true,
        .current_channel9_attempted = true,
        .current_channel9_ret = -1,
        .channel9_require_ret = PROTO_OK,
        .channel9_retry_delay_ms = 31u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);

    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY);
    assert(strcmp(decision.reason, "gateway-ack-current-channel9") == 0);
    assert(decision.delay_ms == 31u);
}

static void test_planned_channel9_event_sends_ack_on_channel9(void)
{
    const struct app_mesh_gateway_ack_state state = {
        .channel9_require_ret = PROTO_OK,
        .channel9_retry_delay_ms = 25u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);

    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_SEND_PLANNED_CHANNEL9);
    assert(strcmp(decision.reason, "gateway-ack") == 0);
}

static void test_stale_or_missing_channel9_requests_route_refresh(void)
{
    struct app_mesh_gateway_ack_state state = {
        .channel9_require_ret = PROTO_ERR_STALE,
        .channel9_retry_delay_ms = 25u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);
    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9);
    assert(strcmp(decision.reason, "gateway-ack-channel9-refresh") == 0);

    state.channel9_require_ret = PROTO_ERR_NOT_FOUND;
    decide(&state, &decision);
    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_REFRESH_CHANNEL9);
    assert(strcmp(decision.reason, "gateway-ack-channel9-refresh") == 0);
}

static void test_busy_channel9_event_waits_without_channel5_ack(void)
{
    const struct app_mesh_gateway_ack_state state = {
        .channel9_require_ret = PROTO_ERR_BUSY,
        .channel9_retry_delay_ms = 42u,
    };
    struct app_mesh_gateway_ack_decision decision;

    decide(&state, &decision);

    assert(decision.action == APP_MESH_GATEWAY_ACK_ACTION_STORE_WAITING_FIXED_RETRY);
    assert(strcmp(decision.reason, "gateway-ack-channel9-wait") == 0);
    assert(decision.delay_ms == 42u);
}

int main(void)
{
    test_route_test_channel9_acks_are_batched();
    test_gateway_rx_on_channel9_tries_current_event_first();
    test_current_channel9_failure_waits_for_channel9_retry();
    test_planned_channel9_event_sends_ack_on_channel9();
    test_stale_or_missing_channel9_requests_route_refresh();
    test_busy_channel9_event_waits_without_channel5_ack();
    return 0;
}
