#ifndef APP_MESH_COORDINATOR_RUNTIME_H
#define APP_MESH_COORDINATOR_RUNTIME_H

#include "app_mesh_coordinator.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Production captures these live sources immediately before arbitration. The
 * runtime derives coordinator inputs so no capture metadata can drift from the
 * decision it is meant to describe.
 */
struct app_mesh_coordinator_runtime_capture {
    bool click_active;
    bool survey_pending;
    uint32_t rx_queue_used;
    uint32_t report_queue_used;
    bool relay_tx_active;
    bool route_waiting_tx_active;
    bool ch9_ack_wait_active;
    bool ch9_ack_send_pending;
    bool gateway_continuous_ch9;
};

struct app_mesh_coordinator_runtime_state {
    bool last_state_valid;
    enum app_mesh_coordinator_state last_state;
};

void app_mesh_coordinator_runtime_reset(
    struct app_mesh_coordinator_runtime_state *state);

int app_mesh_coordinator_runtime_capture_inputs(
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_inputs *inputs);

int app_mesh_coordinator_runtime_decide(
    const struct app_mesh_coordinator_runtime_capture *capture,
    struct app_mesh_coordinator_runtime_state *state,
    struct app_mesh_coordinator_decision *decision,
    bool *state_changed);

#endif
