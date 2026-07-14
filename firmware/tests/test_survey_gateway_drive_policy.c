#include "survey_gateway_transaction.h"

#include <assert.h>
#include <stdio.h>

static enum survey_gateway_drive_action decide(
    bool survey_active,
    bool auto_running,
    bool auto_waiting,
    bool pair_observation_active,
    bool cleanup_pending,
    bool boundary_pending)
{
    const struct survey_gateway_drive_state state = {
        .survey_active = survey_active,
        .auto_running = auto_running,
        .auto_waiting = auto_waiting,
        .pair_observation_active = pair_observation_active,
        .cleanup_pending = cleanup_pending,
        .boundary_pending = boundary_pending,
    };

    return survey_gateway_drive_action(&state);
}

static void test_cleanup_always_keeps_polling(void)
{
    assert(decide(true, true, false, false, true, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
    assert(decide(false, false, false, false, true, false) ==
           SURVEY_GATEWAY_DRIVE_POLL_CLEANUP);
}

static void test_boundary_custody_uses_bounded_retry(void)
{
    assert(decide(true, true, false, false, false, true) ==
           SURVEY_GATEWAY_DRIVE_RETRY_BOUNDARY);
}

static void test_runnable_orphan_is_driven_now(void)
{
    /* Covers both another pair and the final LOAD_PAIR completion step. */
    assert(decide(true, true, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_RUN_NOW);
}

static void test_external_waits_do_not_spin(void)
{
    assert(decide(true, true, true, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
    assert(decide(true, true, false, true, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
    assert(decide(true, false, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
    assert(decide(false, true, false, false, false, false) ==
           SURVEY_GATEWAY_DRIVE_NONE);
}

int main(void)
{
    test_cleanup_always_keeps_polling();
    test_boundary_custody_uses_bounded_retry();
    test_runnable_orphan_is_driven_now();
    test_external_waits_do_not_spin();
    assert(survey_gateway_drive_action(NULL) == SURVEY_GATEWAY_DRIVE_NONE);
    puts("survey gateway drive policy tests passed");
    return 0;
}
