#include "app_mesh_ch9_ack.h"

#include <assert.h>

static void test_ack_complete_keeps_idle_route_test_timing_open(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_does_not_close_when_work_remains(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = true,
        .transmitter_role = false,
        .report_tx_queue_used = 1u,
        .route_waiting_tx_valid = true,
        .ack_batch_valid = true,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
}

static void test_ack_complete_policy_is_disabled_outside_route_test(void)
{
    const struct app_mesh_ch9_ack_complete_state state = {
        .route_test_enabled = false,
        .transmitter_role = false,
        .report_tx_queue_used = 0u,
        .route_waiting_tx_valid = false,
        .ack_batch_valid = false,
    };

    assert(!app_mesh_ch9_ack_complete_should_close_timing(&state));
    assert(!app_mesh_ch9_ack_complete_should_close_timing(NULL));
}

int main(void)
{
    test_ack_complete_keeps_idle_route_test_timing_open();
    test_ack_complete_does_not_close_when_work_remains();
    test_ack_complete_policy_is_disabled_outside_route_test();
    return 0;
}
