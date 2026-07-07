#ifndef APP_MESH_C5_PRIORITY_H
#define APP_MESH_C5_PRIORITY_H

#include "mesh_relay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct app_mesh_c5_flood_priority_state {
    bool response_priority;
    bool anchor_busy;
    bool survey_busy;
    bool gateway_ch5_preempt;
};

struct app_mesh_c5_route_capture_state {
    uint8_t msg_type;
    uint64_t src_id;
    uint64_t dst_id;
    uint64_t previous_hop_id;
    uint64_t target_id;
    uint64_t local_id;
};

struct app_mesh_c5_route_adv_timing {
    uint32_t wake_to_route_delay_ms;
    uint32_t request_flood_burst_ms;
    uint32_t embedded_reply_guard_ms;
    uint32_t route_adv_reply_guard_ms;
};

struct app_mesh_c5_route_reply_window_timing {
    uint32_t base_reply_window_ms;
    uint32_t wake_train_ms;
    uint32_t post_wake_route_rx_ms;
    uint32_t wake_to_route_delay_ms;
    uint32_t request_flood_burst_ms;
    uint32_t flood_forward_wave_ms;
    uint32_t route_reply_exchange_ms;
    uint32_t direct_gateway_probe_ms;
    uint32_t guard_ms;
};

struct app_mesh_c5_connected_gap_timing {
    uint32_t next_channel9_delay_ms;
    uint32_t scan_cap_ms;
    uint32_t min_scan_ms;
    uint32_t retune_margin_ms;
};

bool app_mesh_c5_flood_should_defer(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_gateway_rx_should_yield_to_response(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_route_capture_relevant(
    const struct app_mesh_c5_route_capture_state *state);
bool app_mesh_c5_route_capture_completes_discovery(uint8_t msg_type);
bool app_mesh_c5_route_capture_requires_ack_hold(uint8_t msg_type);
bool app_mesh_c5_control_uses_extended_phr(uint8_t msg_type,
                                           size_t frame_len,
                                           size_t standard_frame_max_len);
bool app_mesh_c5_wake_claim_preempts_mesh(uint8_t claim_flags);
uint32_t app_mesh_c5_route_reply_listen_window_ms(
    uint8_t route_ttl,
    const struct app_mesh_c5_route_reply_window_timing *timing);
uint32_t app_mesh_c5_route_adv_response_delay_ms(
    uint16_t wake_train_ends_in_ms,
    bool embedded_route_frame,
    const struct app_mesh_c5_route_adv_timing *timing);
uint32_t app_mesh_c5_connected_gap_window_ms(
    const struct app_mesh_c5_connected_gap_timing *timing);
uint32_t app_mesh_c5_connected_gap_reschedule_ms(
    uint32_t next_channel9_delay_ms,
    uint32_t min_scan_ms,
    uint32_t retune_margin_ms);

#endif
