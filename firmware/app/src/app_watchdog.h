#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stdint.h>

#define APP_WATCHDOG_HARDWARE_TIMEOUT_MS 180000u
#define APP_WATCHDOG_PROGRESS_LEASE_MS 120000u
#define APP_WATCHDOG_STARTUP_GRACE_MS 30000u

struct app_watchdog_health {
    uint32_t feeds;
    uint32_t stale_system_leases;
    uint32_t stale_radio_leases;
    uint32_t reset_cause;
    int init_error;
};

int app_watchdog_init(void);
void app_watchdog_note_radio_progress(void);
void app_watchdog_stop_feeding(void);
void app_watchdog_get_health(struct app_watchdog_health *health);

#endif
