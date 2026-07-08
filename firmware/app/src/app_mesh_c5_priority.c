#include "app_mesh_c5_priority.h"

#include <stddef.h>

static bool event_control_type(uint8_t msg_type)
{
    return msg_type == MSG_MESH_EVENT_PROPOSE ||
           msg_type == MSG_MESH_EVENT_ACCEPT ||
           msg_type == MSG_MESH_EVENT_UPDATE ||
           msg_type == MSG_MESH_EVENT_END;
}

bool app_mesh_c5_flood_should_defer(
    const struct app_mesh_c5_flood_priority_state *state)
{
    if (state == NULL) {
        return true;
    }

    if (state->anchor_busy || state->survey_busy) {
        return true;
    }

    if (state->gateway_ch5_preempt && !state->response_priority) {
        return true;
    }

    return false;
}

bool app_mesh_c5_gateway_rx_should_yield_to_response(
    const struct app_mesh_c5_flood_priority_state *state)
{
    if (state == NULL) {
        return false;
    }

    return state->response_priority && state->gateway_ch5_preempt &&
           !state->anchor_busy && !state->survey_busy;
}

bool app_mesh_c5_gateway_route_adv_allowed(bool mesh_route_test_enabled)
{
    return !mesh_route_test_enabled;
}

bool app_mesh_c5_route_capture_relevant(
    const struct app_mesh_c5_route_capture_state *state)
{
    if (state == NULL) {
        return false;
    }

    if (state->msg_type == MSG_ROUTE_REPLY) {
        return state->src_id == state->target_id &&
               state->dst_id == state->local_id;
    }

    if (state->msg_type == MSG_GATEWAY_ROUTE_ADV) {
        return state->src_id == state->target_id &&
               state->dst_id == MESH_BROADCAST_ID &&
               state->previous_hop_id != MESH_BROADCAST_ID &&
               state->previous_hop_id != state->local_id;
    }

    if (event_control_type(state->msg_type)) {
        return state->dst_id == state->local_id;
    }

    return false;
}

bool app_mesh_c5_route_capture_completes_discovery(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REPLY ||
           msg_type == MSG_GATEWAY_ROUTE_ADV;
}

bool app_mesh_c5_route_capture_requires_ack_hold(uint8_t msg_type)
{
    return msg_type == MSG_ROUTE_REPLY;
}

bool app_mesh_c5_control_uses_extended_phr(uint8_t msg_type,
                                           size_t frame_len,
                                           size_t standard_frame_max_len)
{
    return frame_len > standard_frame_max_len ||
           msg_type == MSG_ROUTE_REQ ||
           msg_type == MSG_ROUTE_REPLY ||
           msg_type == MSG_GATEWAY_ROUTE_ADV ||
           event_control_type(msg_type);
}

bool app_mesh_c5_wake_claim_preempts_mesh(uint8_t claim_flags)
{
    return (claim_flags & FLAG_COUNT_AS_CLICK) != 0u ||
           (claim_flags & FLAG_RANGE_ONLY) == 0u;
}

uint32_t app_mesh_c5_route_reply_listen_window_ms(
    uint8_t route_ttl,
    const struct app_mesh_c5_route_reply_window_timing *timing)
{
    uint32_t post_wake_route_ms;
    uint32_t window_ms;

    if (timing == NULL) {
        return 0u;
    }

    (void)route_ttl;
    post_wake_route_ms = timing->post_wake_route_rx_ms;
    if (post_wake_route_ms < timing->wake_to_route_delay_ms) {
        post_wake_route_ms = timing->wake_to_route_delay_ms;
    }

    window_ms = timing->direct_gateway_probe_ms +
                post_wake_route_ms +
                timing->route_reply_exchange_ms +
                timing->guard_ms;
    if (window_ms < timing->base_reply_window_ms) {
        return timing->base_reply_window_ms;
    }
    return window_ms;
}

uint32_t app_mesh_c5_route_adv_response_delay_ms(
    uint16_t wake_train_ends_in_ms,
    bool embedded_route_frame,
    const struct app_mesh_c5_route_adv_timing *timing)
{
    if (timing == NULL) {
        return wake_train_ends_in_ms;
    }

    if (embedded_route_frame) {
        return (uint32_t)wake_train_ends_in_ms +
               timing->embedded_reply_guard_ms;
    }

    return (uint32_t)wake_train_ends_in_ms +
           timing->wake_to_route_delay_ms +
           timing->route_adv_reply_guard_ms;
}

uint32_t app_mesh_c5_connected_gap_window_ms(
    const struct app_mesh_c5_connected_gap_timing *timing)
{
    uint32_t available_ms;

    if (timing == NULL ||
        timing->next_channel9_delay_ms <= timing->retune_margin_ms) {
        return 0u;
    }

    available_ms = timing->next_channel9_delay_ms - timing->retune_margin_ms;
    if (available_ms < timing->min_scan_ms) {
        return 0u;
    }
    if (available_ms > timing->scan_cap_ms) {
        return timing->scan_cap_ms;
    }
    return available_ms;
}

uint32_t app_mesh_c5_connected_gap_reschedule_ms(
    uint32_t next_channel9_delay_ms,
    uint32_t min_scan_ms,
    uint32_t retune_margin_ms)
{
    uint32_t available_ms;

    if (next_channel9_delay_ms <= retune_margin_ms) {
        return next_channel9_delay_ms;
    }

    available_ms = next_channel9_delay_ms - retune_margin_ms;
    return available_ms >= min_scan_ms ? 0u : available_ms;
}
