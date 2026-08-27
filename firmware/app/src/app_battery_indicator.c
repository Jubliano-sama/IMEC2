#include "app_battery_indicator.h"

#include "app_board.h"
#include "app_config.h"
#include "battery_status.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

LOG_MODULE_REGISTER(app_battery_indicator, LOG_LEVEL_INF);

#define BATTERY_INDICATOR_LED_ON_MS 50u
#define BATTERY_INDICATOR_ANCHOR_PERIOD_MS 5000u
#define BATTERY_INDICATOR_CLICKER_PERIOD_MS 10000u

BUILD_ASSERT(BATTERY_INDICATOR_LED_ON_MS <
                 BATTERY_INDICATOR_ANCHOR_PERIOD_MS,
             "anchor battery pulse must fit inside its period");
BUILD_ASSERT(BATTERY_INDICATOR_LED_ON_MS <
                 BATTERY_INDICATOR_CLICKER_PERIOD_MS,
             "clicker battery pulse must fit inside its period");

K_MUTEX_DEFINE(battery_indicator_mutex);

static struct k_work_delayable battery_indicator_work;
static bool battery_indicator_initialized;
static bool battery_indicator_suspended = true;
static bool battery_indicator_led_on;
static bool battery_indicator_sample_failure_reported;

static bool battery_indicator_role_enabled(void)
{
    return IS_ENABLED(CONFIG_IMEC_PRODUCTION_BATTERY_INDICATOR) &&
           (DEVICE_ROLE == ROLE_ANCHOR || DEVICE_ROLE == ROLE_CLICKER);
}

static uint32_t battery_indicator_period_ms(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR ?
           BATTERY_INDICATOR_ANCHOR_PERIOD_MS :
           BATTERY_INDICATOR_CLICKER_PERIOD_MS;
}

static void battery_indicator_led_off(void)
{
    status_led0_set(false, false, false);
    battery_indicator_led_on = false;
    if (DEVICE_ROLE == ROLE_CLICKER) {
        status_leds_disconnect();
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
    int ret;

    ARG_UNUSED(work);

    k_mutex_lock(&battery_indicator_mutex, K_FOREVER);
    if (!battery_indicator_initialized || battery_indicator_suspended) {
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }

    period_ms = battery_indicator_period_ms();
    if (battery_indicator_led_on) {
        battery_indicator_led_off();
        (void)battery_indicator_schedule(
            period_ms - BATTERY_INDICATOR_LED_ON_MS);
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }

    ret = battery_sample_lithium_mv(&battery_mv);
    if (ret < 0) {
        if (!battery_indicator_sample_failure_reported) {
            LOG_WRN("battery indicator ADC sample unavailable: %d", ret);
            battery_indicator_sample_failure_reported = true;
        }
        (void)battery_indicator_schedule(period_ms);
        k_mutex_unlock(&battery_indicator_mutex);
        return;
    }
    battery_indicator_sample_failure_reported = false;

    if (DEVICE_ROLE == ROLE_ANCHOR) {
        band = battery_status_anchor_band(battery_mv);
        status_led0_set(band == BATTERY_STATUS_LOW,
                        band == BATTERY_STATUS_HIGH,
                        band == BATTERY_STATUS_MIDDLE);
    } else {
        band = battery_status_clicker_band(battery_mv);
        ret = status_leds_connect();
        if (ret < 0) {
            LOG_WRN("clicker battery LED reconnect failed: %d", ret);
            (void)battery_indicator_schedule(period_ms);
            k_mutex_unlock(&battery_indicator_mutex);
            return;
        }
        status_led0_set(band == BATTERY_STATUS_LOW,
                        band == BATTERY_STATUS_HIGH,
                        band == BATTERY_STATUS_MIDDLE);
    }

    battery_indicator_led_on = true;
    (void)battery_indicator_schedule(BATTERY_INDICATOR_LED_ON_MS);
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
