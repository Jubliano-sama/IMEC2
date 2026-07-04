#include "app_mesh_c5_priority.h"

#include <assert.h>
#include <stdbool.h>

static void test_passive_gateway_preempt_defers_background_flood(void)
{
    const struct app_mesh_c5_flood_priority_state state = {
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_flood_should_defer(&state));
}

static void test_priority_response_bypasses_passive_gateway_preempt(void)
{
    const struct app_mesh_c5_flood_priority_state state = {
        .response_priority = true,
        .gateway_ch5_preempt = true,
    };

    assert(!app_mesh_c5_flood_should_defer(&state));
}

static void test_gateway_rx_yields_to_priority_response(void)
{
    const struct app_mesh_c5_flood_priority_state priority_state = {
        .response_priority = true,
        .gateway_ch5_preempt = true,
    };
    const struct app_mesh_c5_flood_priority_state background_state = {
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_gateway_rx_should_yield_to_response(&priority_state));
    assert(!app_mesh_c5_gateway_rx_should_yield_to_response(&background_state));
}

static void test_protected_anchor_work_still_defers_priority_response(void)
{
    const struct app_mesh_c5_flood_priority_state anchor_state = {
        .response_priority = true,
        .anchor_busy = true,
        .gateway_ch5_preempt = true,
    };
    const struct app_mesh_c5_flood_priority_state survey_state = {
        .response_priority = true,
        .survey_busy = true,
        .gateway_ch5_preempt = true,
    };

    assert(app_mesh_c5_flood_should_defer(&anchor_state));
    assert(app_mesh_c5_flood_should_defer(&survey_state));
}

static void test_idle_state_does_not_defer(void)
{
    const struct app_mesh_c5_flood_priority_state state = {0};

    assert(!app_mesh_c5_flood_should_defer(&state));
}

static void test_gateway_route_adv_counts_as_route_capture(void)
{
    const struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_GATEWAY_ROUTE_ADV,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };

    assert(app_mesh_c5_route_capture_relevant(&state));
    assert(app_mesh_c5_route_capture_completes_discovery(state.msg_type));
}

static void test_unrelated_gateway_route_adv_is_ignored(void)
{
    struct app_mesh_c5_route_capture_state state = {
        .msg_type = MSG_GATEWAY_ROUTE_ADV,
        .src_id = 0x9999888877776666ull,
        .dst_id = MESH_BROADCAST_ID,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x8888888877776666ull,
        .local_id = 0x3333333333333301ull,
    };

    assert(!app_mesh_c5_route_capture_relevant(&state));
    state.src_id = state.target_id;
    state.previous_hop_id = state.local_id;
    assert(!app_mesh_c5_route_capture_relevant(&state));
}

static void test_route_reply_and_event_control_capture_rules(void)
{
    const struct app_mesh_c5_route_capture_state route_reply = {
        .msg_type = MSG_ROUTE_REPLY,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x3333333333333301ull,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };
    const struct app_mesh_c5_route_capture_state event_control = {
        .msg_type = MSG_MESH_EVENT_PROPOSE,
        .src_id = 0x9999888877776666ull,
        .dst_id = 0x3333333333333301ull,
        .previous_hop_id = 0x9999888877776666ull,
        .target_id = 0x9999888877776666ull,
        .local_id = 0x3333333333333301ull,
    };

    assert(app_mesh_c5_route_capture_relevant(&route_reply));
    assert(app_mesh_c5_route_capture_completes_discovery(route_reply.msg_type));
    assert(app_mesh_c5_route_capture_relevant(&event_control));
    assert(!app_mesh_c5_route_capture_completes_discovery(event_control.msg_type));
}

static void test_route_adv_delay_targets_requester_reply_window(void)
{
    const struct app_mesh_c5_route_adv_timing timing = {
        .wake_to_route_delay_ms = 40u,
        .request_flood_burst_ms = 600u,
        .embedded_reply_guard_ms = 5u,
        .route_adv_reply_guard_ms = 20u,
    };

    assert(app_mesh_c5_route_adv_response_delay_ms(315u, false, &timing) ==
           975u);
    assert(app_mesh_c5_route_adv_response_delay_ms(315u, true, &timing) ==
           320u);
}

int main(void)
{
    test_passive_gateway_preempt_defers_background_flood();
    test_priority_response_bypasses_passive_gateway_preempt();
    test_gateway_rx_yields_to_priority_response();
    test_protected_anchor_work_still_defers_priority_response();
    test_idle_state_does_not_defer();
    test_gateway_route_adv_counts_as_route_capture();
    test_unrelated_gateway_route_adv_is_ignored();
    test_route_reply_and_event_control_capture_rules();
    test_route_adv_delay_targets_requester_reply_window();
    return 0;
}
