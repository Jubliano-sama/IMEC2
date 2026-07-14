#ifndef APP_MESH_RX_POLICY_H
#define APP_MESH_RX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "dwm3000_driver.h"

struct app_mesh_rx_handoff_state {
    bool control_active;
    bool scan_radio_active;
};

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type);
bool app_mesh_rx_policy_role_uses_uwb_rx(bool focused_anchor_rx_logs,
                                         bool permanent_receiver_role,
                                         bool mesh_route_test_transmitter,
                                         bool channel9_schedule_installed);
bool app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
    int ret,
    enum dwm3000_rx_failure failure);
uint32_t app_mesh_rx_policy_gateway_ch9_window_ms(
    uint32_t continuous_window_ms,
    bool control_pending,
    uint32_t control_wait_ms);

void app_mesh_rx_handoff_reset(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_begin_control(
    struct app_mesh_rx_handoff_state *state,
    bool *abort_scan);
bool app_mesh_rx_handoff_try_begin_scan(
    struct app_mesh_rx_handoff_state *state);
void app_mesh_rx_handoff_end_scan(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_control_ready(
    const struct app_mesh_rx_handoff_state *state);
void app_mesh_rx_handoff_end_control(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_scan_rearm_allowed(
    const struct app_mesh_rx_handoff_state *state);

#endif
