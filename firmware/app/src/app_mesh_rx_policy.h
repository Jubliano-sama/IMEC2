#ifndef APP_MESH_RX_POLICY_H
#define APP_MESH_RX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "dwm3000_driver.h"

#define APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS 100u
#define APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS 2u
#define APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE 3u

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
uint32_t app_mesh_rx_policy_gateway_ch9_work_slice_ms(
    uint32_t remaining_window_ms);
uint32_t app_mesh_rx_policy_gateway_ch9_rearm_delay_ms(void);
bool app_mesh_rx_policy_gateway_ch9_should_yield_recovery(
    uint16_t recoverable_errors_in_slice);

#endif
