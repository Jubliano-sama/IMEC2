#include "app_mesh_rx_policy.h"

#include "protocol.h"

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type)
{
    return mesh_route_test_transmitter &&
           msg_type == MSG_GATEWAY_ROUTE_ADV;
}
