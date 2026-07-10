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

static void test_permanent_receiver_roles_always_use_uwb_rx(void)
{
    assert(app_mesh_rx_policy_role_uses_uwb_rx(false, true, false, false));
    assert(app_mesh_rx_policy_role_uses_uwb_rx(false, true, true, false));
}

static void test_transmitter_uses_rx_only_with_channel9_schedule(void)
{
    assert(!app_mesh_rx_policy_role_uses_uwb_rx(false, false, true, false));
    assert(app_mesh_rx_policy_role_uses_uwb_rx(false, false, true, true));
    assert(!app_mesh_rx_policy_role_uses_uwb_rx(false, false, false, true));
}

static void test_focused_anchor_logging_disables_background_rx(void)
{
    assert(!app_mesh_rx_policy_role_uses_uwb_rx(true, true, true, true));
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

static void test_gateway_ch9_rx_keeps_full_continuous_window(void)
{
    assert(app_mesh_rx_policy_gateway_ch9_window_ms(30000u,
                                                     false,
                                                     0u) == 30000u);
    assert(app_mesh_rx_policy_gateway_ch9_window_ms(30000u,
                                                     true,
                                                     30000u) == 30000u);
}

static void test_gateway_ch9_rx_clips_only_for_control_work(void)
{
    assert(app_mesh_rx_policy_gateway_ch9_window_ms(30000u,
                                                     true,
                                                     125u) == 125u);
    assert(app_mesh_rx_policy_gateway_ch9_window_ms(30000u,
                                                     true,
                                                     0u) == 1u);
}

int main(void)
{
    test_transmitter_image_ignores_gateway_route_adv();
    test_only_transmitter_image_ignores_gateway_route_adv();
    test_transmitter_image_keeps_other_mesh_packets();
    test_permanent_receiver_roles_always_use_uwb_rx();
    test_transmitter_uses_rx_only_with_channel9_schedule();
    test_focused_anchor_logging_disables_background_rx();
    test_gateway_ch9_rx_recovers_from_sfd_timeout();
    test_gateway_ch9_rx_recovers_from_corrupt_frame();
    test_gateway_ch9_rx_does_not_recover_from_window_timeout();
    test_gateway_ch9_rx_does_not_recover_unknown_eio();
    test_gateway_ch9_rx_keeps_full_continuous_window();
    test_gateway_ch9_rx_clips_only_for_control_work();
    return 0;
}
