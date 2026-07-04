#ifndef APP_MESH_C5_PRIORITY_H
#define APP_MESH_C5_PRIORITY_H

#include "mesh_relay.h"

#include <stdbool.h>
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

bool app_mesh_c5_flood_should_defer(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_gateway_rx_should_yield_to_response(
    const struct app_mesh_c5_flood_priority_state *state);
bool app_mesh_c5_route_capture_relevant(
    const struct app_mesh_c5_route_capture_state *state);
bool app_mesh_c5_route_capture_completes_discovery(uint8_t msg_type);
uint32_t app_mesh_c5_route_adv_response_delay_ms(
    uint16_t wake_train_ends_in_ms,
    bool embedded_route_frame,
    const struct app_mesh_c5_route_adv_timing *timing);

#endif
