#include "app_mesh_rx_policy.h"

#include "protocol.h"

#include <errno.h>
#include <string.h>

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type)
{
    return mesh_route_test_transmitter &&
           msg_type == MSG_GATEWAY_ROUTE_ADV;
}

bool app_mesh_rx_policy_role_uses_uwb_rx(bool focused_anchor_rx_logs,
                                         bool permanent_receiver_role,
                                         bool mesh_route_test_transmitter,
                                         bool channel9_schedule_installed)
{
    if (focused_anchor_rx_logs) {
        return false;
    }
    return permanent_receiver_role ||
           (mesh_route_test_transmitter && channel9_schedule_installed);
}

bool app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
    int ret,
    enum dwm3000_rx_failure failure)
{
    if (ret == -EMSGSIZE) {
        return true;
    }
    if (ret != -EIO) {
        return false;
    }

    switch (failure) {
    case DWM3000_RX_FAILURE_SFD_TIMEOUT:
    case DWM3000_RX_FAILURE_FRAME_TIMEOUT:
    case DWM3000_RX_FAILURE_CRC_OR_PHY:
    case DWM3000_RX_FAILURE_BAD_FRAME:
        return true;
    case DWM3000_RX_FAILURE_NONE:
    case DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT:
    default:
        return false;
    }
}

uint32_t app_mesh_rx_policy_gateway_ch9_window_ms(
    uint32_t continuous_window_ms,
    bool control_pending,
    uint32_t control_wait_ms)
{
    if (!control_pending || control_wait_ms >= continuous_window_ms) {
        return continuous_window_ms;
    }

    return control_wait_ms > 0u ? control_wait_ms : 1u;
}

uint32_t app_mesh_rx_policy_gateway_ch9_work_slice_ms(
    uint32_t remaining_window_ms)
{
    return remaining_window_ms < APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS ?
        remaining_window_ms : APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS;
}

bool app_mesh_rx_policy_gateway_ch9_should_yield_recovery(
    uint16_t recoverable_errors_in_slice)
{
    return recoverable_errors_in_slice >=
           APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE;
}

void app_mesh_rx_handoff_reset(struct app_mesh_rx_handoff_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool app_mesh_rx_handoff_request_scheduled_control(
    struct app_mesh_rx_handoff_state *state,
    bool *abort_scan)
{
    if (state == NULL || abort_scan == NULL) {
        return false;
    }

    state->scheduled_control_pending = true;
    *abort_scan = state->scan_radio_active;
    return true;
}

bool app_mesh_rx_handoff_scheduled_control_pending(
    const struct app_mesh_rx_handoff_state *state)
{
    return state != NULL && state->scheduled_control_pending;
}

bool app_mesh_rx_handoff_scheduled_control_ready(
    const struct app_mesh_rx_handoff_state *state)
{
    return state != NULL && state->scheduled_control_pending &&
           !state->scan_radio_active;
}

bool app_mesh_rx_handoff_end_scheduled_control(
    struct app_mesh_rx_handoff_state *state)
{
    bool was_pending;

    if (state == NULL) {
        return false;
    }
    was_pending = state->scheduled_control_pending;
    state->scheduled_control_pending = false;
    return was_pending;
}

bool app_mesh_rx_handoff_begin_control(
    struct app_mesh_rx_handoff_state *state,
    bool *abort_scan)
{
    if (state == NULL || abort_scan == NULL || state->control_active) {
        return false;
    }

    state->control_active = true;
    *abort_scan = state->scan_radio_active;
    return true;
}

bool app_mesh_rx_handoff_try_begin_scan(
    struct app_mesh_rx_handoff_state *state)
{
    if (state == NULL || state->scheduled_control_pending ||
        state->control_active || state->scan_radio_active) {
        return false;
    }

    state->scan_radio_active = true;
    return true;
}

void app_mesh_rx_handoff_end_scan(struct app_mesh_rx_handoff_state *state)
{
    if (state != NULL) {
        state->scan_radio_active = false;
    }
}

bool app_mesh_rx_handoff_control_ready(
    const struct app_mesh_rx_handoff_state *state)
{
    return state != NULL && state->control_active &&
           !state->scan_radio_active;
}

void app_mesh_rx_handoff_end_control(struct app_mesh_rx_handoff_state *state)
{
    if (state != NULL) {
        state->control_active = false;
    }
}

bool app_mesh_rx_handoff_scan_rearm_allowed(
    const struct app_mesh_rx_handoff_state *state)
{
    return state != NULL && !state->scheduled_control_pending &&
           !state->control_active;
}
