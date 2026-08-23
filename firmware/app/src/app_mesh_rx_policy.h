#ifndef APP_MESH_RX_POLICY_H
#define APP_MESH_RX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "dwm3000_driver.h"

#define APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS 500u
#define APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS 1u
#define APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE 3u

struct app_mesh_rx_handoff_state {
    bool scheduled_control_pending;
    bool control_active;
    bool scan_radio_active;
};

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type);
bool app_mesh_rx_policy_postboot_route_adv_fresh(
    uint8_t msg_type,
    uint32_t message_age_ms,
    uint64_t received_uptime_ms);
bool app_mesh_rx_policy_role_uses_uwb_rx(bool permanent_receiver_role,
                                         bool scheduled_receiver_enabled,
                                         bool channel9_schedule_installed);
bool app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
    int ret,
    enum dwm3000_rx_failure failure);
bool app_mesh_rx_policy_dwm_attempt_made_progress(
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

void app_mesh_rx_handoff_reset(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_request_scheduled_control(
    struct app_mesh_rx_handoff_state *state,
    bool *abort_scan);
bool app_mesh_rx_handoff_scheduled_control_pending(
    const struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_scheduled_control_ready(
    const struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_end_scheduled_control(
    struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_begin_control(
    struct app_mesh_rx_handoff_state *state,
    bool *abort_scan);
bool app_mesh_rx_handoff_try_begin_scan(
    struct app_mesh_rx_handoff_state *state);
void app_mesh_rx_handoff_end_scan(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_control_ready(
    const struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_control_active(
    const struct app_mesh_rx_handoff_state *state);
void app_mesh_rx_handoff_end_control(struct app_mesh_rx_handoff_state *state);
bool app_mesh_rx_handoff_scan_rearm_allowed(
    const struct app_mesh_rx_handoff_state *state);

#endif
