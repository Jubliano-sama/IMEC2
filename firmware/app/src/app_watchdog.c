#include "app_watchdog.h"

#include "app_board.h"
#include "app_config.h"
#include "watchdog_adoption.h"

#include <hal/nrf_wdt.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(app_watchdog, LOG_LEVEL_INF);

#define APP_WATCHDOG_CHECK_MS 1000u

BUILD_ASSERT(APP_WATCHDOG_HARDWARE_TIMEOUT_MS <= 0x07cfffffu,
             "watchdog timeout must fit the nRF hardware counter");
BUILD_ASSERT(APP_WATCHDOG_PROGRESS_LEASE_MS + APP_WATCHDOG_CHECK_MS <
             APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
             "watchdog lease must expire before the hardware timeout");
BUILD_ASSERT(APP_WATCHDOG_INIT_RETRY_DELAY_MS <
             APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
             "watchdog initialization retry must remain bounded");
BUILD_ASSERT(APP_WATCHDOG_TERMINAL_RESTART_MIN_BOOT_MS +
                 APP_WATCHDOG_TERMINAL_RESTART_DELAY_MS <
                 APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
             "terminal restart backoff must fit the production watchdog");

static const struct device *const watchdog_device =
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));
static struct k_timer watchdog_timer;
static struct k_work_delayable system_progress_work;
static atomic_t system_progress_ms;
static atomic_t radio_progress_ms;
static atomic_t clicker_action_generation_counter;
static atomic_t clicker_action_active_generation;
static atomic_t clicker_action_progress_generation;
static atomic_t clicker_action_progress_ms;
static atomic_t feeding_stopped;
static atomic_t bypass_stop_reported;
static uint64_t startup_grace_until_ms;
static bool stale_reported;
static int8_t zephyr_watchdog_channel = -1;
static uint8_t inherited_reload_request_mask;
static struct app_watchdog_health watchdog_health;
/* Unknown reset history must not reopen a rapid durable-boot write loop. */
static bool terminal_restart_boot_backoff = true;

static void watchdog_timer_handler(struct k_timer *timer);
static void watchdog_bypass_feed(void);

static bool clicker_idle_watchdog_coalesced(void)
{
    return DEVICE_ROLE == ROLE_CLICKER &&
           IS_ENABLED(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR);
}

static uint32_t lease_age_ms(uint32_t now_ms, atomic_t *lease)
{
    return (uint32_t)(now_ms - (uint32_t)atomic_get(lease));
}

static bool radio_lease_required(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           DEVICE_ROLE != ROLE_CLICKER;
}

static void system_progress_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    atomic_set(&system_progress_ms, (atomic_val_t)k_uptime_get_32());
    if (clicker_idle_watchdog_coalesced()) {
        watchdog_timer_handler(NULL);
        return;
    }
    (void)k_work_reschedule(&system_progress_work,
                            K_MSEC(APP_WATCHDOG_CHECK_MS));
}

static uint8_t enabled_reload_request_mask(void)
{
    uint8_t mask = 0u;
    uint8_t index;

    for (index = 0u; index < WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS; index++) {
        if (nrf_wdt_reload_request_enable_check(
                NRF_WDT0, (nrf_wdt_rr_register_t)index)) {
            mask |= (uint8_t)(UINT32_C(1) << index);
        }
    }
    return mask;
}

static void feed_inherited_reload_request(uint8_t reload_request, void *ctx)
{
    ARG_UNUSED(ctx);

    nrf_wdt_reload_request_set(
        NRF_WDT0, (nrf_wdt_rr_register_t)reload_request);
}

static uint8_t feed_inherited_watchdog(bool feeding_allowed)
{
    return watchdog_adoption_feed_mask(
        inherited_reload_request_mask,
        WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS,
        feeding_allowed,
        feed_inherited_reload_request,
        NULL);
}

static void start_watchdog_health_monitor(void)
{
    k_work_init_delayable(&system_progress_work,
                          system_progress_work_handler);
    if (clicker_idle_watchdog_coalesced()) {
        return;
    }
    k_timer_init(&watchdog_timer, watchdog_timer_handler, NULL);
    (void)k_work_reschedule(&system_progress_work, K_NO_WAIT);
    k_timer_start(&watchdog_timer,
                  K_MSEC(APP_WATCHDOG_CHECK_MS),
                  K_MSEC(APP_WATCHDOG_CHECK_MS));
}

static void watchdog_timer_handler(struct k_timer *timer)
{
    uint64_t now_uptime_ms = (uint64_t)k_uptime_get();
    uint32_t now_ms = (uint32_t)now_uptime_ms;
    uint32_t system_age_ms;
    uint32_t radio_age_ms;
    uint32_t clicker_action_age_ms;
    uint32_t clicker_action_generation_before;
    uint32_t clicker_action_generation_after;
    uint32_t clicker_action_progress_generation_value;
    uint32_t clicker_action_progress_ms_value;
    bool system_stale;
    bool radio_stale;
    bool clicker_action_stale;

    ARG_UNUSED(timer);

    if (IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)) {
        watchdog_bypass_feed();
        return;
    }
    if (atomic_get(&feeding_stopped) != 0 ||
        (zephyr_watchdog_channel < 0 &&
         inherited_reload_request_mask == 0u)) {
        return;
    }
    system_age_ms = lease_age_ms(now_ms, &system_progress_ms);
    radio_age_ms = lease_age_ms(now_ms, &radio_progress_ms);
    clicker_action_generation_before =
        (uint32_t)atomic_get(&clicker_action_active_generation);
    clicker_action_progress_generation_value =
        (uint32_t)atomic_get(&clicker_action_progress_generation);
    clicker_action_progress_ms_value =
        (uint32_t)atomic_get(&clicker_action_progress_ms);
    clicker_action_generation_after =
        (uint32_t)atomic_get(&clicker_action_active_generation);
    clicker_action_age_ms =
        (uint32_t)(now_ms - clicker_action_progress_ms_value);
    system_stale = system_age_ms > APP_WATCHDOG_PROGRESS_LEASE_MS;
    radio_stale = radio_lease_required() &&
                  radio_age_ms > APP_WATCHDOG_PROGRESS_LEASE_MS;
    clicker_action_stale = app_watchdog_action_lease_stale(
        clicker_action_generation_before,
        clicker_action_generation_after,
        clicker_action_progress_generation_value,
        now_ms,
        clicker_action_progress_ms_value,
        APP_WATCHDOG_PROGRESS_LEASE_MS);
    if (now_uptime_ms < startup_grace_until_ms) {
        system_stale = false;
        radio_stale = false;
        clicker_action_stale = false;
    }
    if (system_stale || radio_stale || clicker_action_stale) {
        if (!stale_reported) {
            LOG_ERR("watchdog progress stale: system_age_ms=%u radio_age_ms=%u clicker_action_age_ms=%u clicker_action_generation=%u system_stale=%u radio_stale=%u clicker_action_stale=%u lease_ms=%u",
                    system_age_ms,
                    radio_age_ms,
                    clicker_action_age_ms,
                    clicker_action_generation_before,
                    system_stale ? 1u : 0u,
                    radio_stale ? 1u : 0u,
                    clicker_action_stale ? 1u : 0u,
                    APP_WATCHDOG_PROGRESS_LEASE_MS);
            stale_reported = true;
        }
        if (system_stale && watchdog_health.stale_system_leases < UINT32_MAX) {
            watchdog_health.stale_system_leases++;
        }
        if (radio_stale && watchdog_health.stale_radio_leases < UINT32_MAX) {
            watchdog_health.stale_radio_leases++;
        }
        if (clicker_action_stale &&
            watchdog_health.stale_clicker_action_leases < UINT32_MAX) {
            watchdog_health.stale_clicker_action_leases++;
        }
        return;
    }
    if (stale_reported) {
        LOG_INF("watchdog progress recovered: system_age_ms=%u radio_age_ms=%u clicker_action_age_ms=%u",
                system_age_ms,
                radio_age_ms,
                clicker_action_age_ms);
        stale_reported = false;
    }
    if (inherited_reload_request_mask != 0u) {
        if (feed_inherited_watchdog(true) == 0u) {
            return;
        }
    } else if (wdt_feed(watchdog_device, zephyr_watchdog_channel) != 0) {
        return;
    }
    if (watchdog_health.feeds < UINT32_MAX) {
        watchdog_health.feeds++;
    }
}

static void watchdog_bypass_feed(void)
{
    if (inherited_reload_request_mask == 0u ||
        feed_inherited_watchdog(true) == 0u) {
        return;
    }
    if (watchdog_health.feeds < UINT32_MAX) {
        watchdog_health.feeds++;
    }
}

int app_watchdog_init(void)
{
    const struct wdt_timeout_cfg timeout = {
        .window = {
            .min = 0u,
            .max = APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };
    struct watchdog_adoption_plan adoption;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t inherited_crv;
    uint8_t enabled_mask;
    bool hardware_running;
    int ret;

    memset(&watchdog_health, 0, sizeof(watchdog_health));
    ret = hwinfo_get_reset_cause(&watchdog_health.reset_cause);
    /* nRF52833 POR/BOR reports no RESETREAS bits. Accept only an exact cold
     * or pin reset; mixed, unknown and failed observations stay conservative.
     * A watchdog reset may corrupt retained RAM, so no RAM breadcrumb is
     * needed or trusted when selecting this boot's recovery policy. */
    terminal_restart_boot_backoff =
        ret != 0 || (watchdog_health.reset_cause != 0u &&
                     watchdog_health.reset_cause != RESET_PIN);
    if (hwinfo_clear_reset_cause() != 0) {
        terminal_restart_boot_backoff = true;
    }
    atomic_set(&system_progress_ms, (atomic_val_t)now_ms);
    atomic_set(&radio_progress_ms, (atomic_val_t)now_ms);
    atomic_clear(&clicker_action_generation_counter);
    atomic_clear(&clicker_action_active_generation);
    atomic_clear(&clicker_action_progress_generation);
    atomic_set(&clicker_action_progress_ms, (atomic_val_t)now_ms);
    atomic_clear(&feeding_stopped);
    atomic_clear(&bypass_stop_reported);
    stale_reported = false;
    startup_grace_until_ms =
        (uint64_t)k_uptime_get() + APP_WATCHDOG_STARTUP_GRACE_MS;
    zephyr_watchdog_channel = -1;
    inherited_reload_request_mask = 0u;

    hardware_running = nrf_wdt_started_check(NRF_WDT0);
    enabled_mask = enabled_reload_request_mask();
    ret = watchdog_adoption_plan(hardware_running,
                                 enabled_mask,
                                 WATCHDOG_ADOPTION_MAX_RELOAD_REQUESTS,
                                 &adoption);
    if (ret < 0) {
        watchdog_health.init_error = ret;
        status_debug_printf("DBG_WATCHDOG_BOOT mode=invalid running=%u rr=0x%02x count=0 immediate=0 reset=0x%08x err=%d\n",
                            hardware_running ? 1u : 0u,
                            (unsigned int)enabled_mask,
                            watchdog_health.reset_cause,
                            ret);
        return ret;
    }
    if (IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)) {
        inherited_crv = 0u;
        if (adoption.mode == WATCHDOG_ADOPTION_INHERITED) {
            inherited_reload_request_mask =
                (uint8_t)adoption.reload_request_mask;
            inherited_crv = nrf_wdt_reload_value_get(NRF_WDT0);
            if (feed_inherited_watchdog(true) !=
                adoption.reload_request_count) {
                watchdog_health.init_error = -EIO;
                inherited_reload_request_mask = 0u;
                return -EIO;
            }
            watchdog_health.feeds = 1u;
            if (!clicker_idle_watchdog_coalesced()) {
                k_timer_init(&watchdog_timer, watchdog_timer_handler, NULL);
                k_timer_start(&watchdog_timer,
                              K_MSEC(APP_WATCHDOG_CHECK_MS),
                              K_MSEC(APP_WATCHDOG_CHECK_MS));
            }
        }
        status_debug_printf(
            "DBG_WATCHDOG_BOOT mode=bypass running=%u rr=0x%02x count=%u immediate=%u crv=%u reset=0x%08x err=0\n",
            hardware_running ? 1u : 0u,
            (unsigned int)inherited_reload_request_mask,
            (unsigned int)adoption.reload_request_count,
            adoption.mode == WATCHDOG_ADOPTION_INHERITED ? 1u : 0u,
            inherited_crv,
            watchdog_health.reset_cause);
        LOG_WRN("watchdog reset policy bypassed for supervised bench capture: inherited=%u rr_mask=0x%02x",
                hardware_running ? 1u : 0u,
                (unsigned int)inherited_reload_request_mask);
        return 0;
    }
    if (adoption.mode == WATCHDOG_ADOPTION_INHERITED) {
        inherited_reload_request_mask =
            (uint8_t)adoption.reload_request_mask;
        inherited_crv = nrf_wdt_reload_value_get(NRF_WDT0);
        if (feed_inherited_watchdog(true) !=
            adoption.reload_request_count) {
            watchdog_health.init_error = -EIO;
            inherited_reload_request_mask = 0u;
            return -EIO;
        }
        watchdog_health.feeds = 1u;
        start_watchdog_health_monitor();
        status_debug_printf("DBG_WATCHDOG_BOOT mode=inherited rr=0x%02x count=%u immediate=1 crv=%u reset=0x%08x err=0\n",
                            (unsigned int)inherited_reload_request_mask,
                            (unsigned int)adoption.reload_request_count,
                            inherited_crv,
                            watchdog_health.reset_cause);
        LOG_INF("inherited hardware watchdog adopted: rr_mask=0x%02x rr_count=%u crv=%u lease_ms=%u reset_cause=0x%08x",
                (unsigned int)inherited_reload_request_mask,
                (unsigned int)adoption.reload_request_count,
                inherited_crv,
                APP_WATCHDOG_PROGRESS_LEASE_MS,
                watchdog_health.reset_cause);
        return 0;
    }

    if (watchdog_device == NULL || !device_is_ready(watchdog_device)) {
        watchdog_health.init_error = -ENODEV;
        return -ENODEV;
    }
    ret = wdt_install_timeout(watchdog_device, &timeout);
    if (ret < 0) {
        watchdog_health.init_error = ret;
        return ret;
    }
    zephyr_watchdog_channel = (int8_t)ret;
    ret = wdt_setup(watchdog_device, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        watchdog_health.init_error = ret;
        zephyr_watchdog_channel = -1;
        return ret;
    }

    start_watchdog_health_monitor();
    status_debug_printf("DBG_WATCHDOG_BOOT mode=fresh rr=0x%02x count=1 immediate=0 crv=%u reset=0x%08x err=0\n",
                        (unsigned int)(UINT32_C(1) <<
                                       (uint8_t)zephyr_watchdog_channel),
                        nrf_wdt_reload_value_get(NRF_WDT0),
                        watchdog_health.reset_cause);
    LOG_INF("hardware watchdog active: timeout_ms=%u lease_ms=%u reset_cause=0x%08x",
            APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
            APP_WATCHDOG_PROGRESS_LEASE_MS,
            watchdog_health.reset_cause);
    return 0;
}

void app_watchdog_clicker_idle_checkpoint(void)
{
    if (!clicker_idle_watchdog_coalesced()) {
        return;
    }

    /* The production clicker is already awake for its battery
     * pulse. Refresh the system-workqueue lease and service the hardware
     * watchdog at that same boundary, so watchdog supervision adds no
     * one-second timer or workqueue wakeups while retained-idle. */
    atomic_set(&system_progress_ms, (atomic_val_t)k_uptime_get_32());
    watchdog_timer_handler(NULL);
}

bool app_watchdog_clicker_action_checkpoint(uint32_t generation)
{
    uint32_t active_generation =
        (uint32_t)atomic_get(&clicker_action_active_generation);

    if (generation == 0u || active_generation != generation ||
        atomic_get(&feeding_stopped) != 0 ||
        app_watchdog_action_lease_stale(
            active_generation,
            (uint32_t)atomic_get(&clicker_action_active_generation),
            (uint32_t)atomic_get(&clicker_action_progress_generation),
            k_uptime_get_32(),
            (uint32_t)atomic_get(&clicker_action_progress_ms),
            APP_WATCHDOG_PROGRESS_LEASE_MS)) {
        return false;
    }
    if (!clicker_idle_watchdog_coalesced()) {
        return true;
    }
    if (IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)) {
        watchdog_bypass_feed();
        return true;
    }

    /* Actions can indefinitely postpone the battery pulse. Queue one check
     * at their completed boundary: the system worker must actually run before
     * it may refresh its own lease and pass the normal watchdog health gate. */
    return k_work_reschedule(&system_progress_work, K_NO_WAIT) >= 0;
}

void app_watchdog_note_radio_progress(void)
{
    atomic_set(&radio_progress_ms, (atomic_val_t)k_uptime_get_32());
}

uint32_t app_watchdog_clicker_action_begin(void)
{
    uint32_t generation;
    uint32_t now_ms;

    if (atomic_get(&clicker_action_active_generation) != 0) {
        return 0u;
    }
    do {
        generation =
            (uint32_t)atomic_inc(&clicker_action_generation_counter) + 1u;
    } while (generation == 0u);

    now_ms = k_uptime_get_32();
    atomic_set(&clicker_action_progress_ms, (atomic_val_t)now_ms);
    atomic_set(&clicker_action_progress_generation,
               (atomic_val_t)generation);
    if (!atomic_cas(&clicker_action_active_generation,
                    0,
                    (atomic_val_t)generation)) {
        return 0u;
    }
    return generation;
}

bool app_watchdog_note_clicker_action_progress(uint32_t generation)
{
    if (generation == 0u ||
        (uint32_t)atomic_get(&clicker_action_active_generation) !=
            generation) {
        return false;
    }

    atomic_set(&clicker_action_progress_ms,
               (atomic_val_t)k_uptime_get_32());
    atomic_set(&clicker_action_progress_generation,
               (atomic_val_t)generation);
    return (uint32_t)atomic_get(&clicker_action_active_generation) ==
           generation;
}

bool app_watchdog_clicker_action_end(uint32_t generation)
{
    if (generation == 0u) {
        return false;
    }
    return atomic_cas(&clicker_action_active_generation,
                      (atomic_val_t)generation,
                      0);
}

void app_watchdog_stop_feeding(void)
{
    if (IS_ENABLED(CONFIG_IMEC_WATCHDOG_BYPASS)) {
        if (atomic_cas(&bypass_stop_reported, 0, 1)) {
            status_debug_printf(
                "DBG_WATCHDOG_BYPASS_STOP_IGNORED uptime=%u\n",
                k_uptime_get_32());
        }
        return;
    }
    atomic_set(&feeding_stopped, 1);
}

static atomic_t terminal_restart_scheduled;

static void terminal_restart_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    /* A timer runs independently of the workqueue that may own the fault. */
    sys_reboot(SYS_REBOOT_COLD);
}

K_TIMER_DEFINE(terminal_restart_timer, terminal_restart_handler, NULL);

void app_watchdog_schedule_terminal_restart(void)
{
    if (atomic_cas(&terminal_restart_scheduled, 0, 1)) {
        uint64_t now_ms = (uint64_t)k_uptime_get();
        uint32_t delay_ms = APP_WATCHDOG_TERMINAL_RESTART_DELAY_MS;

        if (terminal_restart_boot_backoff &&
            now_ms < APP_WATCHDOG_TERMINAL_RESTART_MIN_BOOT_MS) {
            uint32_t remaining_ms =
                APP_WATCHDOG_TERMINAL_RESTART_MIN_BOOT_MS - (uint32_t)now_ms;

            if (remaining_ms > delay_ms) {
                delay_ms = remaining_ms;
            }
        }
        /* Feeds remain stopped. An inherited shorter hardware timeout can
         * preempt this delay once; its watchdog reset also resets the WDT,
         * allowing the next boot to install the normal one-hour timeout. */
        k_timer_start(&terminal_restart_timer,
                      K_MSEC(delay_ms),
                      K_NO_WAIT);
    }
}

void app_watchdog_get_health(struct app_watchdog_health *health)
{
    if (health != NULL) {
        *health = watchdog_health;
    }
}
