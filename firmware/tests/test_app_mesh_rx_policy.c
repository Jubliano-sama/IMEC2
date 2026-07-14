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

static void test_control_handoff_aborts_active_scan_and_blocks_rearm(void)
{
    struct app_mesh_rx_handoff_state state;
    bool abort_scan = false;

    app_mesh_rx_handoff_reset(&state);
    assert(app_mesh_rx_handoff_try_begin_scan(&state));
    assert(app_mesh_rx_handoff_begin_control(&state, &abort_scan));
    assert(abort_scan);
    assert(!app_mesh_rx_handoff_scan_rearm_allowed(&state));
    assert(!app_mesh_rx_handoff_try_begin_scan(&state));
    assert(!app_mesh_rx_handoff_control_ready(&state));

    app_mesh_rx_handoff_end_scan(&state);
    assert(app_mesh_rx_handoff_control_ready(&state));
    assert(!app_mesh_rx_handoff_try_begin_scan(&state));

    app_mesh_rx_handoff_end_control(&state);
    assert(app_mesh_rx_handoff_scan_rearm_allowed(&state));
    assert(app_mesh_rx_handoff_try_begin_scan(&state));
}

static void test_control_handoff_wins_before_scan_acquires_radio(void)
{
    struct app_mesh_rx_handoff_state state;
    bool abort_scan = true;

    app_mesh_rx_handoff_reset(&state);
    assert(app_mesh_rx_handoff_begin_control(&state, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_rx_handoff_control_ready(&state));
    assert(!app_mesh_rx_handoff_try_begin_scan(&state));
    assert(!app_mesh_rx_handoff_begin_control(&state, &abort_scan));
    app_mesh_rx_handoff_end_control(&state);
    assert(app_mesh_rx_handoff_try_begin_scan(&state));
}

static void test_scheduled_control_idle_gate_blocks_scan_until_delivery_ends(void)
{
    struct app_mesh_rx_handoff_state state;
    bool abort_scan = true;

    app_mesh_rx_handoff_reset(&state);
    assert(app_mesh_rx_handoff_request_scheduled_control(&state,
                                                          &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_rx_handoff_scheduled_control_pending(&state));
    assert(app_mesh_rx_handoff_scheduled_control_ready(&state));
    assert(!app_mesh_rx_handoff_scan_rearm_allowed(&state));
    assert(!app_mesh_rx_handoff_try_begin_scan(&state));

    assert(app_mesh_rx_handoff_end_scheduled_control(&state));
    assert(!app_mesh_rx_handoff_scheduled_control_pending(&state));
    assert(app_mesh_rx_handoff_scan_rearm_allowed(&state));
    assert(!app_mesh_rx_handoff_end_scheduled_control(&state));
}

static void test_scheduled_control_aborts_active_scan_and_coalesces(void)
{
    struct app_mesh_rx_handoff_state state;
    bool abort_scan = false;

    app_mesh_rx_handoff_reset(&state);
    assert(app_mesh_rx_handoff_try_begin_scan(&state));
    assert(app_mesh_rx_handoff_request_scheduled_control(&state,
                                                          &abort_scan));
    assert(abort_scan);
    assert(!app_mesh_rx_handoff_scheduled_control_ready(&state));
    assert(!app_mesh_rx_handoff_try_begin_scan(&state));

    app_mesh_rx_handoff_end_scan(&state);
    assert(app_mesh_rx_handoff_scheduled_control_ready(&state));
    abort_scan = true;
    assert(app_mesh_rx_handoff_request_scheduled_control(&state,
                                                          &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_rx_handoff_scheduled_control_pending(&state));
    assert(app_mesh_rx_handoff_end_scheduled_control(&state));
    assert(app_mesh_rx_handoff_scan_rearm_allowed(&state));
}

static void test_host_control_can_run_ahead_of_scheduled_control(void)
{
    struct app_mesh_rx_handoff_state state;
    bool abort_scan = true;

    app_mesh_rx_handoff_reset(&state);
    assert(app_mesh_rx_handoff_begin_control(&state, &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_rx_handoff_request_scheduled_control(&state,
                                                          &abort_scan));
    assert(!abort_scan);
    assert(app_mesh_rx_handoff_scheduled_control_ready(&state));
    assert(!app_mesh_rx_handoff_scan_rearm_allowed(&state));

    app_mesh_rx_handoff_end_control(&state);
    assert(app_mesh_rx_handoff_scheduled_control_pending(&state));
    assert(!app_mesh_rx_handoff_scan_rearm_allowed(&state));
    assert(app_mesh_rx_handoff_begin_control(&state, &abort_scan));
    app_mesh_rx_handoff_end_control(&state);
    assert(app_mesh_rx_handoff_end_scheduled_control(&state));
    assert(app_mesh_rx_handoff_scan_rearm_allowed(&state));
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
    test_control_handoff_aborts_active_scan_and_blocks_rearm();
    test_control_handoff_wins_before_scan_acquires_radio();
    test_scheduled_control_idle_gate_blocks_scan_until_delivery_ends();
    test_scheduled_control_aborts_active_scan_and_coalesces();
    test_host_control_can_run_ahead_of_scheduled_control();
    return 0;
}
