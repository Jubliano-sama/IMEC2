#include "app_battery_indicator.h"

#include "app_board.h"
#include "app_config.h"
#include "app_watchdog.h"
#include "battery_status.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_battery_indicator, LOG_LEVEL_INF);

#define BATTERY_INDICATOR_LED_ON_MS 50u
#define BATTERY_INDICATOR_ANCHOR_PERIOD_MS 5000u
#define BATTERY_INDICATOR_CLICKER_PERIOD_MS 5000u
#define BATTERY_INDICATOR_CLICKER_LED_ON_MS 25u
#define BATTERY_INDICATOR_CLICKER_RED_ON_MS 15u
#define BATTERY_INDICATOR_CLICKER_SAMPLE_MS 3600000u
#define BATTERY_INDICATOR_USB_PERIOD_MS 1000u
#define BATTERY_INDICATOR_USB_LED_ON_MS 100u
#define BATTERY_INDICATOR_USB_SAMPLE_MS 30000u
#define BATTERY_INDICATOR_RECOVERY_REBOOT_DELAY_MS 1000u

BUILD_ASSERT(BATTERY_INDICATOR_LED_ON_MS <
                 BATTERY_INDICATOR_ANCHOR_PERIOD_MS,
             "anchor battery pulse must fit inside its period");
BUILD_ASSERT(BATTERY_INDICATOR_CLICKER_RED_ON_MS <=
                 BATTERY_INDICATOR_CLICKER_LED_ON_MS &&
                 BATTERY_INDICATOR_CLICKER_LED_ON_MS <
                 BATTERY_INDICATOR_CLICKER_PERIOD_MS,
             "clicker battery pulse must fit inside its period");
BUILD_ASSERT(BATTERY_INDICATOR_CLICKER_PERIOD_MS <
                 APP_WATCHDOG_HARDWARE_TIMEOUT_MS,
             "clicker battery wake must service the watchdog before timeout");

BUILD_ASSERT(BATTERY_INDICATOR_USB_LED_ON_MS < BATTERY_INDICATOR_USB_PERIOD_MS,
             "USB battery pulse must fit inside its period");

K_MUTEX_DEFINE(battery_indicator_mutex);

static struct k_work_delayable battery_indicator_work;
static bool battery_indicator_initialized;
static bool battery_indicator_suspended = true;
static bool battery_indicator_led_on;
static bool battery_indicator_sample_failure_reported;
static uint16_t battery_indicator_cached_mv;
static uint64_t battery_indicator_sampled_ms;
static bool battery_indicator_refresh_due;
static uint32_t battery_indicator_pulse_ms;
static uint32_t battery_indicator_cycle_ms;
static bool battery_indicator_usb_present;

static bool battery_indicator_role_enabled(void)
{
    return IS_ENABLED(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR) &&
           (DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_CLICKER);
}

static uint32_t battery_indicator_period_ms(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR ?
           BATTERY_INDICATOR_ANCHOR_PERIOD_MS :
           (battery_indicator_usb_present ? BATTERY_INDICATOR_USB_PERIOD_MS :
            BATTERY_INDICATOR_CLICKER_PERIOD_MS);
}

static void battery_indicator_led_off(void)
{
    status_led0_set(false, false, false);
    battery_indicator_led_on = false;
    if (DEVICE_ROLE == ROLE_CLICKER) {
        status_led1_set(false, false, false);
        status_leds_disconnect();
    }
}

static void battery_indicator_recovery_reset(int sample_ret, int cleanup_ret)
{
    LOG_ERR("battery divider off state unproven after bounded recovery: sample=%d cleanup=%d",
            sample_ret, cleanup_ret);
    app_watchdog_stop_feeding();
    LOG_PANIC();
    k_msleep(BATTERY_INDICATOR_RECOVERY_REBOOT_DELAY_MS);
    sys_reboot(SYS_REBOOT_COLD);
    for (;;) {
        k_cpu_idle();
    }
}

static bool battery_indicator_schedule(uint32_t delay_ms)
{
    int ret = k_work_reschedule(&battery_indicator_work, K_MSEC(delay_ms));

    if (ret < 0) {
        LOG_ERR("battery indicator lost its periodic work owner: %d", ret);
        if (battery_indicator_led_on) {
            battery_indicator_led_off();
        }
        return false;
    }
    return true;
}

static void battery_indicator_work_handler(struct k_work *work)
{
    enum battery_status_band band;
    uint32_t period_ms;
    uint16_t battery_mv = 0u;
    bool power_known = true;
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&battery_indicator_mutex, K_FOREVER);
    if (!battery_indicator_initialized || battery_indicator_suspended) {
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }

    if (battery_indicator_led_on) {
        battery_indicator_led_off();
        (void)battery_indicator_schedule(
            battery_indicator_cycle_ms - battery_indicator_pulse_ms);
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }

    if (DEVICE_ROLE == ROLE_CLICKER) {
        app_watchdog_clicker_idle_checkpoint();
        int usb_ret = battery_usb_power_present();
        bool present = usb_ret > 0;

        power_known = usb_ret >= 0;
        if (!power_known || present != battery_indicator_usb_present) {
            battery_indicator_refresh_due = true;
        }
        battery_indicator_usb_present = present;
    }
    period_ms = battery_indicator_period_ms();
    battery_indicator_cycle_ms = period_ms;

    battery_mv = battery_indicator_cached_mv;
    ret = 0;
    if (DEVICE_ROLE == ROLE_ANCHOR || battery_indicator_refresh_due ||
        (uint64_t)k_uptime_get() - battery_indicator_sampled_ms >=
            (battery_indicator_usb_present ? BATTERY_INDICATOR_USB_SAMPLE_MS :
             BATTERY_INDICATOR_CLICKER_SAMPLE_MS)) {
        ret = battery_sample_lithium_mv(&battery_mv);
        if (ret == 0) {
            battery_indicator_cached_mv = battery_mv;
            battery_indicator_sampled_ms = (uint64_t)k_uptime_get();
            battery_indicator_refresh_due = !power_known;
        }
    }
    if (ret < 0) {
        /* A negative sample can include partial enable or failed cleanup.
         * Prove off again before an ordinary ADC retry returns to idle. The
         * board helper already bounds its GPIO retries. */
        int cleanup_ret = battery_adc_divider_disable();

        if (cleanup_ret < 0) {
            battery_indicator_suspended = true;
            battery_indicator_recovery_reset(ret, cleanup_ret);
            return;
        }
        if (!battery_indicator_sample_failure_reported) {
            LOG_WRN("battery indicator ADC sample unavailable: %d", ret);
            battery_indicator_sample_failure_reported = true;
        }
        (void)battery_indicator_schedule(period_ms);
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }
    battery_indicator_sample_failure_reported = false;

    battery_indicator_pulse_ms = BATTERY_INDICATOR_LED_ON_MS;
    if (DEVICE_ROLE == ROLE_ANCHOR) {
        band = battery_status_anchor_band(battery_mv);
        status_led0_set(band == BATTERY_STATUS_LOW,
                        band == BATTERY_STATUS_HIGH,
                        band == BATTERY_STATUS_MIDDLE);
    } else {
        band = battery_status_clicker_band(battery_mv);
        battery_indicator_pulse_ms = battery_indicator_usb_present ?
            BATTERY_INDICATOR_USB_LED_ON_MS :
            (band == BATTERY_STATUS_LOW ? BATTERY_INDICATOR_CLICKER_RED_ON_MS :
             BATTERY_INDICATOR_CLICKER_LED_ON_MS);
        ret = status_leds_connect();
        if (ret < 0) {
            LOG_WRN("clicker battery LED reconnect failed: %d", ret);
            (void)battery_indicator_schedule(period_ms);
            k_mutex_unlock(&battery_indicator_mutex);
            return;
        }
        if (battery_indicator_usb_present) {
            bool low = band == BATTERY_STATUS_LOW;
            bool high = battery_mv > 4000u;

            status_led0_set(low, high, !low && !high);
            status_led1_set(false, battery_mv > 4150u, false);
        } else {
            status_led0_set(band == BATTERY_STATUS_LOW,
                            band == BATTERY_STATUS_HIGH,
                            band == BATTERY_STATUS_MIDDLE);
            status_led1_set(false, false, false);
        }
    }

    battery_indicator_led_on = true;
    (void)battery_indicator_schedule(battery_indicator_pulse_ms);
    k_mutex_unlock(&battery_indicator_mutex);
}

int app_battery_indicator_init(void)
{
    if (!battery_indicator_role_enabled()) {
        return 0;
    }
    if (battery_indicator_initialized) {
        return -EALREADY;
    }

    k_work_init_delayable(&battery_indicator_work,
                          battery_indicator_work_handler);
    battery_indicator_initialized = true;
    battery_indicator_suspended = true;
    battery_indicator_led_on = false;
    battery_indicator_sample_failure_reported = false;
    battery_indicator_refresh_due = true;
    return 0;
}

void app_battery_indicator_resume(void)
{
    if (!battery_indicator_role_enabled() ||
        !battery_indicator_initialized) {
        return;
    }

    k_mutex_lock(&battery_indicator_mutex, K_FOREVER);
    if (battery_indicator_suspended) {
        battery_indicator_suspended = false;
        /* Boot and completed action bursts refresh once on the next pulse. */
        battery_indicator_refresh_due = true;
        (void)battery_indicator_schedule(battery_indicator_period_ms());
    }
    k_mutex_unlock(&battery_indicator_mutex);
}

void app_battery_indicator_suspend(void)
{
    if (!battery_indicator_role_enabled() ||
        !battery_indicator_initialized) {
        return;
    }

    k_mutex_lock(&battery_indicator_mutex, K_FOREVER);
    battery_indicator_suspended = true;
    (void)k_work_cancel_delayable(&battery_indicator_work);
    if (battery_indicator_led_on) {
        battery_indicator_led_off();
    }
    k_mutex_unlock(&battery_indicator_mutex);
}
