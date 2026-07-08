#include "app_mesh_rx_policy.h"

#include "protocol.h"

#include <assert.h>
#include <errno.h>

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

static void test_gateway_ch9_rx_recovers_from_sfd_timeout(void)
{
    assert(app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
        -EIO,
        DWM3000_RX_FAILURE_SFD_TIMEOUT));
}

static void test_gateway_ch9_rx_recovers_from_corrupt_frame(void)
{
    assert(app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
        -EIO,
        DWM3000_RX_FAILURE_CRC_OR_PHY));
    assert(app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
        -EMSGSIZE,
        DWM3000_RX_FAILURE_BAD_FRAME));
}

static void test_gateway_ch9_rx_does_not_recover_from_window_timeout(void)
{
    assert(!app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
        -ETIMEDOUT,
        DWM3000_RX_FAILURE_NO_PREAMBLE_TIMEOUT));
}

static void test_gateway_ch9_rx_does_not_recover_unknown_eio(void)
{
    assert(!app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
        -EIO,
        DWM3000_RX_FAILURE_NONE));
}

int main(void)
{
    test_transmitter_image_ignores_gateway_route_adv();
    test_only_transmitter_image_ignores_gateway_route_adv();
    test_transmitter_image_keeps_other_mesh_packets();
    test_gateway_ch9_rx_recovers_from_sfd_timeout();
    test_gateway_ch9_rx_recovers_from_corrupt_frame();
    test_gateway_ch9_rx_does_not_recover_from_window_timeout();
    test_gateway_ch9_rx_does_not_recover_unknown_eio();
    return 0;
}
