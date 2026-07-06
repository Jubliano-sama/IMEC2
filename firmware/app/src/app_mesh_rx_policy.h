#ifndef APP_MESH_RX_POLICY_H
#define APP_MESH_RX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

bool app_mesh_rx_policy_should_drop(bool mesh_route_test_transmitter,
                                    uint8_t msg_type);

#endif
