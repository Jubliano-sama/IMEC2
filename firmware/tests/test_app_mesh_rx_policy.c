#include "app_mesh_rx_policy.h"

#include "protocol.h"

#include <assert.h>

static void test_transmitter_image_ignores_gateway_route_adv(void)
{
    assert(app_mesh_rx_policy_should_drop(true,
                                          MSG_GATEWAY_ROUTE_ADV));
}

static void test_only_transmitter_image_ignores_gateway_route_adv(void)
{
    assert(!app_mesh_rx_policy_should_drop(false,
                                           MSG_GATEWAY_ROUTE_ADV));
}

static void test_transmitter_image_keeps_other_mesh_packets(void)
{
    assert(!app_mesh_rx_policy_should_drop(true,
                                           MSG_ROUTE_REQ));
    assert(!app_mesh_rx_policy_should_drop(true,
                                           MSG_GATEWAY_ACK));
}

int main(void)
{
    test_transmitter_image_ignores_gateway_route_adv();
    test_only_transmitter_image_ignores_gateway_route_adv();
    test_transmitter_image_keeps_other_mesh_packets();
    return 0;
}
