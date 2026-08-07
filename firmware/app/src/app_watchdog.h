#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* PoC: watchdog disabled. Values kept only so BUILD_ASSERTs elsewhere still
 * parse; they are not enforced when CONFIG_WATCHDOG=n. Re-enable for
 * production after end-to-end enumeration/survey/mesh is proven. */
#define APP_WATCHDOG_HARDWARE_TIMEOUT_MS 180000u
#define APP_WATCHDOG_PROGRESS_LEASE_MS 120000u
#define APP_WATCHDOG_STARTUP_GRACE_MS 30000u
#define APP_WATCHDOG_INIT_RETRY_DELAY_MS 1000u

struct app_watchdog_health {
    uint32_t feeds;
    uint32_t stale_system_leases;
    uint32_t stale_radio_leases;
    uint32_t stale_clicker_action_leases;
    uint32_t reset_cause;
    int init_error;
};

/*
 * A stable active generation must have progress from that exact generation.
 * A generation transition during the snapshot is retried on the next monitor
 * tick rather than being mistaken for a stalled action.
 */
static inline bool app_watchdog_action_lease_stale(
    uint32_t active_generation_before,
    uint32_t active_generation_after,
    uint32_t progress_generation,
    uint32_t now_ms,
    uint32_t progress_ms,
    uint32_t lease_ms)
{
    if (active_generation_before == 0u ||
        active_generation_before != active_generation_after) {
        return false;
    }
    return progress_generation != active_generation_before ||
           (uint32_t)(now_ms - progress_ms) > lease_ms;
}

int app_watchdog_init(void);
void app_watchdog_note_radio_progress(void);
uint32_t app_watchdog_clicker_action_begin(void);
bool app_watchdog_note_clicker_action_progress(uint32_t generation);
bool app_watchdog_clicker_action_end(uint32_t generation);
void app_watchdog_stop_feeding(void);
void app_watchdog_get_health(struct app_watchdog_health *health);

#endif
