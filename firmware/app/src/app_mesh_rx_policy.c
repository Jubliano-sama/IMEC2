#include "app_mesh_rx_policy.h"

#include "protocol.h"

#include <errno.h>

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type)
{
    return mesh_route_test_transmitter &&
           msg_type == MSG_GATEWAY_ROUTE_ADV;
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
