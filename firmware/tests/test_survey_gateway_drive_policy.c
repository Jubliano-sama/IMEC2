#include "survey_gateway_transaction.h"

#include <assert.h>
#include <stdio.h>

static enum survey_gateway_drive_action decide(
    bool survey_active,
    bool control_inflight,
    bool round_observing,
    bool round_drive_ready,
    bool cleanup_pending,
    bool boundary_pending)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = survey_active,
        .control_inflight = control_inflight,
        .round_observing = round_observing,
        .round_drive_ready = round_drive_ready,
        .cleanup_pending = cleanup_pending,
        .boundary_pending = boundary_pending,
    };

    return survey_gateway_drive_action(&state);
}

static void test_cleanup_always_keeps_polling(void)
{
    assert(decide(true, false, false, false, true, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
    assert(decide(false, false, false, false, true, true) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
}

static void test_boundary_custody_uses_bounded_retry(void)
{
    assert(decide(true, false, false, false, false, true) ==
           SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY);
}

static void test_runnable_orphan_is_driven_now(void)
{
    /* Covers both another pair and the final LOAD_PAIR completion step. */
    assert(decide(true, false, false, true, false, false) ==
           SURVEY_GATEWAY_DRIVE_RUN_NOW);
}

static void test_round_drive_ready_does_not_depend_on_serial_auto_owner(void)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = true,
        .control_inflight = false,
        .round_observing = false,
        .round_drive_ready = true,
        .cleanup_pending = false,
        .boundary_pending = false,
    };

    assert(survey_gateway_drive_action(&state) ==
           SURVEY_GATEWAY_DRIVE_RUN_NOW);
}

static void test_observing_round_polls_without_busy_spin(void)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = true,
        .control_inflight = false,
        .round_observing = true,
        .round_drive_ready = false,
        .cleanup_pending = false,
        .boundary_pending = false,
    };

    assert(survey_gateway_drive_action(&state) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
}

static void test_confirmation_wait_does_not_redrive_current_control(void)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = true,
        .control_inflight = false,
        .control_confirmation_pending = true,
        .round_observing = false,
        /* DISPATCHING remains true until the exact confirmation advances it. */
        .round_drive_ready = true,
        .cleanup_pending = false,
        .boundary_pending = false,
    };

    assert(survey_gateway_drive_action(&state) ==
           SURVEY_GATEWAY_DRIVE_WAIT_CONFIRMATION);
}

static void test_external_waits_keep_a_bounded_deadline_poll(void)
{
    assert(decide(true, true, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
    assert(decide(true, false, true, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_WAIT);
    assert(decide(true, false, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
    assert(decide(false, false, false, true, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
}

int main(void)
{
    test_cleanup_always_keeps_polling();
    test_boundary_custody_uses_bounded_retry();
    test_runnable_orphan_is_driven_now();
    test_round_drive_ready_does_not_depend_on_serial_auto_owner();
    test_observing_round_polls_without_busy_spin();
    test_confirmation_wait_does_not_redrive_current_control();
    test_external_waits_keep_a_bounded_deadline_poll();
    assert(survey_gateway_drive_action(NULL) == SURVEY_GATEWAY_DRIVE_NONE);
    puts("survey gateway drive policy tests passed");
    return 0;
}
