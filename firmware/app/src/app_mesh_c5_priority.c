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

    if (state->gateway_ack_pending) {
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
    uint32_t request_forward_waves;
    uint32_t reply_hops;
    uint32_t post_wake_route_ms;
    uint32_t per_forward_wave_ms;
    uint32_t window_ms;

    if (timing == NULL) {
        return 0u;
    }

    request_forward_waves = route_ttl > 0u ? (uint32_t)route_ttl - 1u : 0u;
    reply_hops = route_ttl > 0u ? route_ttl : 1u;
    post_wake_route_ms = timing->post_wake_route_rx_ms;
    if (post_wake_route_ms <
        timing->wake_to_route_delay_ms + timing->request_flood_burst_ms) {
        post_wake_route_ms =
            timing->wake_to_route_delay_ms + timing->request_flood_burst_ms;
    }

    per_forward_wave_ms = timing->flood_forward_wave_ms +
                          timing->wake_train_ms +
                          post_wake_route_ms;
    window_ms = timing->direct_gateway_probe_ms +
                (request_forward_waves * per_forward_wave_ms) +
                (reply_hops * timing->route_reply_exchange_ms) +
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
           timing->request_flood_burst_ms +
           timing->route_adv_reply_guard_ms;
}
