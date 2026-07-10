#include "app_mesh_rx_policy.h"

#include "protocol.h"

#include <errno.h>

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
