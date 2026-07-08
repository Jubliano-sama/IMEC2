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

static void test_mesh_route_test_gateway_does_not_advertise_on_channel5(void)
{
    assert(!app_mesh_c5_gateway_route_adv_allowed(true));
    assert(app_mesh_c5_gateway_route_adv_allowed(false));
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
    assert(!app_mesh_c5_route_capture_requires_ack_hold(state.msg_type));
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
    assert(app_mesh_c5_route_capture_requires_ack_hold(route_reply.msg_type));
    assert(app_mesh_c5_route_capture_relevant(&event_control));
    assert(!app_mesh_c5_route_capture_completes_discovery(event_control.msg_type));
    assert(!app_mesh_c5_route_capture_requires_ack_hold(event_control.msg_type));
}

static void test_channel5_control_phr_policy(void)
{
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REQ, 146u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REQ, 95u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REPLY, 95u, 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_GATEWAY_ROUTE_ADV,
                                                 114u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_MESH_EVENT_PROPOSE,
                                                 103u,
                                                 125u));
    assert(app_mesh_c5_control_uses_extended_phr(MSG_MESH_EVENT_ACCEPT,
                                                 103u,
                                                 125u));
    assert(!app_mesh_c5_control_uses_extended_phr(MSG_ROUTE_REPLY_ACK,
                                                  95u,
                                                  125u));
}

static void test_wake_claim_click_priority_policy(void)
{
    assert(!app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC |
                                                 FLAG_RANGE_ONLY));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC));
    assert(app_mesh_c5_wake_claim_preempts_mesh(0u));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_COUNT_AS_CLICK));
    assert(app_mesh_c5_wake_claim_preempts_mesh(FLAG_DIAGNOSTIC |
                                                FLAG_COUNT_AS_CLICK));
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
           375u);
    assert(app_mesh_c5_route_adv_response_delay_ms(315u, true, &timing) ==
           320u);
}

static void test_route_reply_window_covers_direct_probe_and_reply_exchange(void)
{
    const struct app_mesh_c5_route_reply_window_timing timing = {
        .base_reply_window_ms = 1000u,
        .wake_train_ms = 400u,
        .post_wake_route_rx_ms = 800u,
        .wake_to_route_delay_ms = 40u,
        .request_flood_burst_ms = 600u,
        .flood_forward_wave_ms = 1400u,
        .route_reply_exchange_ms = 1375u,
        .direct_gateway_probe_ms = 200u,
        .guard_ms = 250u,
    };
    uint32_t ttl1_window =
        app_mesh_c5_route_reply_listen_window_ms(1u, &timing);
    uint32_t ttl2_window =
        app_mesh_c5_route_reply_listen_window_ms(2u, &timing);
    uint32_t ttl4_window =
        app_mesh_c5_route_reply_listen_window_ms(4u, &timing);
    uint32_t ttl8_window =
        app_mesh_c5_route_reply_listen_window_ms(8u, &timing);

    assert(ttl1_window == 2625u);
    assert(ttl2_window == 2625u);
    assert(ttl4_window == 2625u);
    assert(ttl8_window == 2625u);
    assert(ttl1_window > timing.base_reply_window_ms);
}

static void test_connected_gap_window_uses_channel5_until_retune_guard(void)
{
    const struct app_mesh_c5_connected_gap_timing long_gap = {
        .next_channel9_delay_ms = 220u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };
    const struct app_mesh_c5_connected_gap_timing short_gap = {
        .next_channel9_delay_ms = 45u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };
    const struct app_mesh_c5_connected_gap_timing capped_gap = {
        .next_channel9_delay_ms = 75u,
        .scan_cap_ms = 100u,
        .min_scan_ms = 20u,
        .retune_margin_ms = 30u,
    };

    assert(app_mesh_c5_connected_gap_window_ms(&long_gap) == 100u);
    assert(app_mesh_c5_connected_gap_window_ms(&short_gap) == 0u);
    assert(app_mesh_c5_connected_gap_window_ms(&capped_gap) == 45u);
}

static void test_connected_gap_reschedules_immediate_c5_until_ch9_is_close(void)
{
    assert(app_mesh_c5_connected_gap_reschedule_ms(220u, 20u, 30u) == 0u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(45u, 20u, 30u) == 15u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(30u, 20u, 30u) == 30u);
    assert(app_mesh_c5_connected_gap_reschedule_ms(0u, 20u, 30u) == 0u);
}

int main(void)
{
    test_passive_gateway_preempt_defers_background_flood();
    test_priority_response_bypasses_passive_gateway_preempt();
    test_gateway_rx_yields_to_priority_response();
    test_mesh_route_test_gateway_does_not_advertise_on_channel5();
    test_protected_anchor_work_still_defers_priority_response();
    test_idle_state_does_not_defer();
    test_gateway_route_adv_counts_as_route_capture();
    test_unrelated_gateway_route_adv_is_ignored();
    test_route_reply_and_event_control_capture_rules();
    test_channel5_control_phr_policy();
    test_wake_claim_click_priority_policy();
    test_route_adv_delay_targets_requester_reply_window();
    test_route_reply_window_covers_direct_probe_and_reply_exchange();
    test_connected_gap_window_uses_channel5_until_retune_guard();
    test_connected_gap_reschedules_immediate_c5_until_ch9_is_close();
    return 0;
}
