#ifndef APP_MESH_SCHEDULER_LIVENESS_H
#define APP_MESH_SCHEDULER_LIVENESS_H

#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum app_mesh_schedule_recovery {
    APP_MESH_SCHEDULE_RECOVERY_NONE = 0,
    APP_MESH_SCHEDULE_RECOVERY_ON_RESUME,
    APP_MESH_SCHEDULE_RECOVERY_WATCHDOG,
};

/*
 * Accepted mesh state always retains its own queue, pending record, or
 * deferred latch before it reaches this decision. A pause-owned -ESHUTDOWN is
 * therefore recoverable by transport resume; every other rejected handoff
 * needs the independent hardware-watchdog recovery owner.
 */
static inline enum app_mesh_schedule_recovery
app_mesh_schedule_recovery_decide(int schedule_ret, bool transport_paused)
{
    if (schedule_ret >= 0) {
        return APP_MESH_SCHEDULE_RECOVERY_NONE;
    }
    if (schedule_ret == -ESHUTDOWN && transport_paused) {
        return APP_MESH_SCHEDULE_RECOVERY_ON_RESUME;
    }
    return APP_MESH_SCHEDULE_RECOVERY_WATCHDOG;
}

/*
 * A wrapped uint32_t deadline may be zero. Keep ownership in a separate flag
 * and merge an intervening immediate kick without shortening the wait.
 */
static inline uint32_t app_mesh_schedule_preserve_deadline(
    bool *deadline_active,
    uint32_t deadline_ms,
    uint32_t now_ms,
    uint32_t requested_delay_ms)
{
    uint32_t remaining_ms;

    if (deadline_active == NULL || !*deadline_active) {
        return requested_delay_ms;
    }
    if ((int32_t)(now_ms - deadline_ms) >= 0) {
        *deadline_active = false;
        return requested_delay_ms;
    }

    remaining_ms = deadline_ms - now_ms;
    return requested_delay_ms < remaining_ms ?
           remaining_ms : requested_delay_ms;
}

#endif
