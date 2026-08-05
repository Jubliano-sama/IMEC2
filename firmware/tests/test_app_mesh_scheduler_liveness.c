#include "app_mesh_scheduler_liveness.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

static void test_successful_submission_needs_no_recovery(void)
{
    assert(app_mesh_schedule_recovery_decide(0, false) ==
           APP_MESH_SCHEDULE_RECOVERY_NONE);
    assert(app_mesh_schedule_recovery_decide(1, false) ==
           APP_MESH_SCHEDULE_RECOVERY_NONE);
}

static void test_paused_shutdown_defers_every_retained_owner_to_resume(void)
{
    static const char *const retained_owners[] = {
        "report-queue",
        "rx-queue",
        "c5-flood",
        "route-action",
        "route-discovery",
        "route-wait",
        "tx-timeout",
        "role-scan",
    };

    for (size_t i = 0u;
         i < sizeof(retained_owners) / sizeof(retained_owners[0]);
         i++) {
        assert(retained_owners[i] != NULL);
        assert(app_mesh_schedule_recovery_decide(-ESHUTDOWN, true) ==
               APP_MESH_SCHEDULE_RECOVERY_ON_RESUME);
    }
}

static void test_nonpause_or_nonshutdown_rejection_forces_recovery(void)
{
    static const int rejected_results[] = {
        -EIO,
        -ENOMEM,
        -EBUSY,
        -ESHUTDOWN,
    };

    for (size_t i = 0u;
         i < sizeof(rejected_results) / sizeof(rejected_results[0]);
         i++) {
        assert(app_mesh_schedule_recovery_decide(
                   rejected_results[i], false) ==
               APP_MESH_SCHEDULE_RECOVERY_WATCHDOG);
    }
    assert(app_mesh_schedule_recovery_decide(-EIO, true) ==
           APP_MESH_SCHEDULE_RECOVERY_WATCHDOG);
}

static void test_wrapped_zero_backoff_deadline_remains_owned(void)
{
    bool active = true;

    assert(app_mesh_schedule_preserve_deadline(
               &active, 0u, UINT32_MAX - 9u, 0u) == 10u);
    assert(active);
    assert(app_mesh_schedule_preserve_deadline(
               &active, 0u, UINT32_MAX - 9u, 12u) == 12u);
    assert(active);
    assert(app_mesh_schedule_preserve_deadline(
               &active, 0u, 0u, 0u) == 0u);
    assert(!active);
    assert(app_mesh_schedule_preserve_deadline(
               &active, 0u, 1u, 7u) == 7u);
}

int main(void)
{
    test_successful_submission_needs_no_recovery();
    test_paused_shutdown_defers_every_retained_owner_to_resume();
    test_nonpause_or_nonshutdown_rejection_forces_recovery();
    test_wrapped_zero_backoff_deadline_remains_owned();
    puts("app mesh scheduler liveness tests passed");
    return 0;
}
