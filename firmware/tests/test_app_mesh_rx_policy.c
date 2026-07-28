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

static void test_gateway_ch9_rx_bounds_one_workqueue_occupancy_slice(void)
{
    const uint32_t logical_window_ms = 30000u;
    uint16_t immediate_error_count = 0u;

    assert(APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS == 100u);
    assert(APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS == 2u);
    assert(APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS > 0u);
    assert(app_mesh_rx_policy_gateway_ch9_rearm_delay_ms() ==
           APP_MESH_RX_GATEWAY_CH9_COOPERATIVE_YIELD_MS);
    assert(APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS < logical_window_ms);
    assert(app_mesh_rx_policy_gateway_ch9_work_slice_ms(
               logical_window_ms) ==
           APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS);
    assert(app_mesh_rx_policy_gateway_ch9_work_slice_ms(
               APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS - 1u) ==
           APP_MESH_RX_GATEWAY_CH9_WORK_SLICE_MS - 1u);
    assert(app_mesh_rx_policy_gateway_ch9_work_slice_ms(0u) == 0u);

    assert(APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE == 3u);
    assert(!app_mesh_rx_policy_gateway_ch9_should_yield_recovery(0u));
    while (!app_mesh_rx_policy_gateway_ch9_should_yield_recovery(
        immediate_error_count)) {
        /*
         * Model a driver that returns immediately without advancing uptime.
         * The count guard, rather than the 100 ms wall-clock deadline, must
         * still yield the shared system workqueue after bounded iterations.
         */
        assert(app_mesh_rx_policy_gateway_ch9_rx_error_recoverable(
            -EIO,
            (immediate_error_count & 1u) != 0u ?
                DWM3000_RX_FAILURE_BAD_FRAME :
                DWM3000_RX_FAILURE_CRC_OR_PHY));
        immediate_error_count++;
        assert(immediate_error_count <=
               APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE);
    }
    assert(immediate_error_count ==
           APP_MESH_RX_GATEWAY_CH9_MAX_RECOVERABLE_ERRORS_PER_SLICE);
    assert(app_mesh_rx_policy_gateway_ch9_should_yield_recovery(
        immediate_error_count));
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
    test_gateway_ch9_rx_bounds_one_workqueue_occupancy_slice();
    return 0;
}
