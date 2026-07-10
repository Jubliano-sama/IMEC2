#include "app_watchdog.h"

#include "app_config.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_watchdog, LOG_LEVEL_INF);

#define APP_WATCHDOG_TIMEOUT_MS 10000u
#define APP_WATCHDOG_CHECK_MS 1000u
#define APP_WATCHDOG_LEASE_MAX_AGE_MS 5000u
#define APP_WATCHDOG_STARTUP_GRACE_MS 15000u

BUILD_ASSERT(APP_WATCHDOG_LEASE_MAX_AGE_MS + APP_WATCHDOG_CHECK_MS <
             APP_WATCHDOG_TIMEOUT_MS,
             "watchdog lease must expire before the hardware timeout");

static const struct device *const watchdog_device =
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
static struct k_timer watchdog_timer;
static struct k_work_delayable system_progress_work;
static atomic_t system_progress_ms;
static atomic_t radio_progress_ms;
static atomic_t feeding_stopped;
static uint32_t startup_grace_until_ms;
static int watchdog_channel = -1;
static struct app_watchdog_health watchdog_health;

static bool lease_stale(uint32_t now_ms, atomic_t *lease)
{
    return (uint32_t)(now_ms - (uint32_t)atomic_get(lease)) >
           APP_WATCHDOG_LEASE_MAX_AGE_MS;
}

static bool radio_lease_required(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           !IS_ENABLED(CONFIG_IMEC_GATEWAY_BLE_CONNECTIVITY_TEST) &&
           DEVICE_ROLE != ROLE_CLICKER;
}

static void system_progress_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    atomic_set(&system_progress_ms, (atomic_val_t)k_uptime_get_32());
    (void)k_work_reschedule(&system_progress_work,
                            K_MSEC(APP_WATCHDOG_CHECK_MS));
}

static void watchdog_timer_handler(struct k_timer *timer)
{
    uint32_t now_ms = k_uptime_get_32();
    bool system_stale;
    bool radio_stale;

    ARG_UNUSED(timer);

    if (atomic_get(&feeding_stopped) != 0 || watchdog_channel < 0) {
        return;
    }
    system_stale = lease_stale(now_ms, &system_progress_ms);
    radio_stale = radio_lease_required() && lease_stale(now_ms, &radio_progress_ms);
    if ((int32_t)(now_ms - startup_grace_until_ms) < 0) {
        system_stale = false;
        radio_stale = false;
    }
    if (system_stale || radio_stale) {
        if (system_stale && watchdog_health.stale_system_leases < UINT32_MAX) {
            watchdog_health.stale_system_leases++;
        }
        if (radio_stale && watchdog_health.stale_radio_leases < UINT32_MAX) {
            watchdog_health.stale_radio_leases++;
        }
        return;
    }
    if (wdt_feed(watchdog_device, watchdog_channel) == 0 &&
        watchdog_health.feeds < UINT32_MAX) {
        watchdog_health.feeds++;
    }
}

int app_watchdog_init(void)
{
    const struct wdt_timeout_cfg timeout = {
        .window = {
            .min = 0u,
            .max = APP_WATCHDOG_TIMEOUT_MS,
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };
    uint32_t now_ms = k_uptime_get_32();
    int ret;

    memset(&watchdog_health, 0, sizeof(watchdog_health));
    (void)hwinfo_get_reset_cause(&watchdog_health.reset_cause);
    atomic_set(&system_progress_ms, (atomic_val_t)now_ms);
    atomic_set(&radio_progress_ms, (atomic_val_t)now_ms);
    atomic_clear(&feeding_stopped);
    startup_grace_until_ms = now_ms + APP_WATCHDOG_STARTUP_GRACE_MS;

    if (watchdog_device == NULL || !device_is_ready(watchdog_device)) {
        watchdog_health.init_error = -ENODEV;
        return -ENODEV;
    }
    watchdog_channel = wdt_install_timeout(watchdog_device, &timeout);
    if (watchdog_channel < 0) {
        watchdog_health.init_error = watchdog_channel;
        return watchdog_channel;
    }
    ret = wdt_setup(watchdog_device, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        watchdog_health.init_error = ret;
        watchdog_channel = -1;
        return ret;
    }

    k_work_init_delayable(&system_progress_work,
                          system_progress_work_handler);
    k_timer_init(&watchdog_timer, watchdog_timer_handler, NULL);
    (void)k_work_reschedule(&system_progress_work, K_NO_WAIT);
    k_timer_start(&watchdog_timer,
                  K_MSEC(APP_WATCHDOG_CHECK_MS),
                  K_MSEC(APP_WATCHDOG_CHECK_MS));
    LOG_INF("hardware watchdog active: timeout_ms=%u reset_cause=0x%08x",
            APP_WATCHDOG_TIMEOUT_MS,
            watchdog_health.reset_cause);
    return 0;
}

void app_watchdog_note_radio_progress(void)
{
    atomic_set(&radio_progress_ms, (atomic_val_t)k_uptime_get_32());
}

void app_watchdog_stop_feeding(void)
{
    atomic_set(&feeding_stopped, 1);
}

void app_watchdog_get_health(struct app_watchdog_health *health)
{
    if (health != NULL) {
        *health = watchdog_health;
    }
}
