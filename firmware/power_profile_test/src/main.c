#include "dwm3000_driver.h"
#include "dwm3000_port.h"
#include "uwb.h"
#include "uwb_session.h"

#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STATUS0_RED_NODE DT_ALIAS(status0_red)
#define STATUS0_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS0_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS1_RED_NODE DT_ALIAS(status1_red)
#define STATUS1_GREEN_NODE DT_ALIAS(status1_green)
#define STATUS1_BLUE_NODE DT_ALIAS(status1_blue)
#define BATTERY_ADC_ENABLE_NODE DT_ALIAS(battery_adc_enable)
#define CLICK_BUTTON_NODE DT_ALIAS(click_button)

#define SPI3_CS_PIN 3u
#define SPI3_SCK_PIN 11u
#define SPI3_MISO_PIN 12u
#define SPI3_MOSI_PIN 0u
#define DWM3000_WAKE_PIN 30u
#define DWM3000_RESET_PIN 31u
#define CLICK_BUTTON_PIN 26u

#ifndef POWER_PROFILE_BOOT_SETTLE_MS
#define POWER_PROFILE_BOOT_SETTLE_MS 5000u
#endif
#ifndef POWER_PROFILE_CLICK_PERIOD_MS
#define POWER_PROFILE_CLICK_PERIOD_MS 10000u
#endif
#ifndef POWER_PROFILE_CLICK_RX_TIMEOUT_MS
#define POWER_PROFILE_CLICK_RX_TIMEOUT_MS 80u
#endif
#ifndef POWER_PROFILE_INTER_TARGET_GAP_MS
#define POWER_PROFILE_INTER_TARGET_GAP_MS 0u
#endif
#ifndef POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS
#define POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS 1000u
#endif
#ifndef POWER_PROFILE_ANCHOR_RX_WINDOW_MS
#define POWER_PROFILE_ANCHOR_RX_WINDOW_MS 40u
#endif
#ifndef POWER_PROFILE_RESPONDER_RX_WINDOW_MS
#define POWER_PROFILE_RESPONDER_RX_WINDOW_MS 250u
#endif
#ifndef POWER_PROFILE_LED_MARKERS
#define POWER_PROFILE_LED_MARKERS 0
#endif
#ifndef POWER_PROFILE_DEBUG_RTT
#define POWER_PROFILE_DEBUG_RTT 0
#endif
#ifndef POWER_PROFILE_CAPTURE_RSL
#define POWER_PROFILE_CAPTURE_RSL 1
#endif
#ifndef POWER_PROFILE_TARGET_COUNT
#define POWER_PROFILE_TARGET_COUNT 1u
#endif
#ifndef POWER_PROFILE_WAKE_TX_TIMEOUT_MS
#define POWER_PROFILE_WAKE_TX_TIMEOUT_MS 80u
#endif
#ifndef POWER_PROFILE_BUTTON_RELEASE_TIMEOUT_MS
#define POWER_PROFILE_BUTTON_RELEASE_TIMEOUT_MS 3000u
#endif
#ifndef POWER_PROFILE_BUTTON_RELEASE_STABLE_MS
#define POWER_PROFILE_BUTTON_RELEASE_STABLE_MS 50u
#endif
#ifndef POWER_PROFILE_BUTTON_RELEASE_POLL_MS
#define POWER_PROFILE_BUTTON_RELEASE_POLL_MS 5u
#endif

#define POWER_PROFILE_RANGE_FLAGS FLAG_DIAGNOSTIC
#define POWER_PROFILE_FRAME_BUF_LEN 128u
#define POWER_PROFILE_WAKE_CLAIM_TIMING_MS 25u
#define POWER_PROFILE_WAKE_CLAIM_DURATION_MS 100u

#if POWER_PROFILE_DEBUG_RTT != 0
#define PROFILE_DBG(...) printk(__VA_ARGS__)
#else
#define PROFILE_DBG(...) do { } while (0)
#endif

BUILD_ASSERT(POWER_PROFILE_TARGET_COUNT >= 1u,
             "POWER_PROFILE_TARGET_COUNT must be at least 1");
BUILD_ASSERT(POWER_PROFILE_TARGET_COUNT <= 4u,
             "POWER_PROFILE_TARGET_COUNT must be 1..4");

static const struct gpio_dt_spec status_leds[] = {
    GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS0_GREEN_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_RED_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_GREEN_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_BLUE_NODE, gpios),
};
#if defined(POWER_PROFILE_MODE_ANCHOR_LOW_DUTY) || \
    defined(POWER_PROFILE_MODE_CLICKER_CLICK) || \
    defined(POWER_PROFILE_MODE_RESPONDER)
static const struct gpio_dt_spec marker_led =
    GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios);
#endif
static const struct gpio_dt_spec error_led =
    GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios);
static const struct gpio_dt_spec battery_adc_enable =
    GPIO_DT_SPEC_GET(BATTERY_ADC_ENABLE_NODE, gpios);
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *const gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

#if defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF)
static const struct gpio_dt_spec click_button =
    GPIO_DT_SPEC_GET(CLICK_BUTTON_NODE, gpios);
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_CLICK) || defined(POWER_PROFILE_MODE_RESPONDER)
static uint8_t seq;
#endif

static void sleep_ms(uint32_t ms)
{
    k_sleep(K_MSEC(ms));
}

static void sleep_forever(void)
{
    while (true) {
        k_sleep(K_FOREVER);
    }
}

static bool reset_reason_was_systemoff(void)
{
#if NRF_POWER_HAS_RESETREAS
    uint32_t reset_reason = nrf_power_resetreas_get(NRF_POWER);

    if (reset_reason != 0u) {
        nrf_power_resetreas_clear(NRF_POWER, reset_reason);
    }
    return (reset_reason & NRF_POWER_RESETREAS_OFF_MASK) != 0u;
#else
    return false;
#endif
}

static void set_output_if_ready(const struct gpio_dt_spec *gpio, bool active)
{
    if (gpio_is_ready_dt(gpio)) {
        (void)gpio_pin_set_dt(gpio, active ? 1 : 0);
    }
}

#if defined(POWER_PROFILE_MODE_ANCHOR_LOW_DUTY) || \
    defined(POWER_PROFILE_MODE_CLICKER_CLICK) || \
    defined(POWER_PROFILE_MODE_RESPONDER)
static void marker_set(bool active)
{
    if (POWER_PROFILE_LED_MARKERS != 0) {
        set_output_if_ready(&marker_led, active);
    } else {
        ARG_UNUSED(active);
    }
}
#endif

static void error_halt(void)
{
    if (POWER_PROFILE_LED_MARKERS != 0) {
        set_output_if_ready(&error_led, true);
    }
    sleep_forever();
}

static void configure_quiet_gpios(void)
{
    for (size_t i = 0u; i < ARRAY_SIZE(status_leds); ++i) {
        if (gpio_is_ready_dt(&status_leds[i])) {
            (void)gpio_pin_configure_dt(&status_leds[i], GPIO_OUTPUT_INACTIVE);
        }
    }

    if (gpio_is_ready_dt(&battery_adc_enable)) {
        (void)gpio_pin_configure(battery_adc_enable.port, battery_adc_enable.pin,
                                 GPIO_OUTPUT_HIGH);
        (void)gpio_pin_set_raw(battery_adc_enable.port,
                               battery_adc_enable.pin, 1);
    }
}

static void disconnect_pin(const struct device *port, gpio_pin_t pin)
{
    if (device_is_ready(port)) {
        (void)gpio_pin_configure(port, pin, GPIO_DISCONNECTED);
    }
}

static void park_for_systemoff(bool button_wake)
{
    for (size_t i = 0u; i < ARRAY_SIZE(status_leds); ++i) {
        if (gpio_is_ready_dt(&status_leds[i])) {
            (void)gpio_pin_configure_dt(&status_leds[i], GPIO_DISCONNECTED);
        }
    }

    if (gpio_is_ready_dt(&battery_adc_enable)) {
        (void)gpio_pin_configure(battery_adc_enable.port, battery_adc_enable.pin,
                                 GPIO_OUTPUT_HIGH);
        (void)gpio_pin_set_raw(battery_adc_enable.port,
                               battery_adc_enable.pin, 1);
    }

    disconnect_pin(gpio0, SPI3_CS_PIN);
    disconnect_pin(gpio0, SPI3_SCK_PIN);
    disconnect_pin(gpio0, SPI3_MISO_PIN);
    disconnect_pin(gpio1, SPI3_MOSI_PIN);
    disconnect_pin(gpio0, DWM3000_WAKE_PIN);
    disconnect_pin(gpio0, DWM3000_RESET_PIN);

#if defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF)
    if (!button_wake && gpio_is_ready_dt(&click_button)) {
        (void)gpio_pin_configure_dt(&click_button, GPIO_DISCONNECTED);
    }
#else
    ARG_UNUSED(button_wake);
    disconnect_pin(gpio0, 26u);
#endif
}

static int radio_init_and_configure_range(void)
{
    uint32_t dev_id;
    int ret;

    ret = dwm3000_port_init();
    if (ret < 0) {
        PROFILE_DBG("radio_init port_init ret=%d\n", ret);
        return ret;
    }
    PROFILE_DBG("radio_init port_init ok\n");

    ret = dwm3000_port_wakeup();
    if (ret < 0) {
        PROFILE_DBG("radio_init wakeup ret=%d\n", ret);
        return ret;
    }
    PROFILE_DBG("radio_init wakeup ok\n");

    ret = dwm3000_port_hw_reset();
    if (ret < 0) {
        PROFILE_DBG("radio_init hw_reset ret=%d\n", ret);
        return ret;
    }
    PROFILE_DBG("radio_init hw_reset ok\n");

    ret = dwm3000_driver_probe(&dev_id);
    if (ret < 0) {
        PROFILE_DBG("radio_init probe ret=%d\n", ret);
        return ret;
    }
    PROFILE_DBG("radio_init probe ok dev_id=0x%08x\n", dev_id);

    ret = dwm3000_driver_configure_range_mode();
    PROFILE_DBG("radio_init range_mode ret=%d\n", ret);
    return ret;
}

#if !defined(POWER_PROFILE_MODE_RESPONDER)
static void standby_or_halt(void)
{
    if (dwm3000_driver_standby() < 0) {
        error_halt();
    }
}
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_CLICK) || defined(POWER_PROFILE_MODE_RESPONDER)
static void fill_range_request(struct dwm3000_range_request *request,
                               uint64_t responder_id)
{
    memset(request, 0, sizeof(*request));
    request->initiator_id = POWER_PROFILE_CLICKER_ID;
    request->responder_id = responder_id;
    request->network_id = POWER_PROFILE_NETWORK_ID;
    request->session_nonce = POWER_PROFILE_SESSION_NONCE;
    request->responder_short_addr = uwb_session_short_addr_from_id(responder_id);
    request->session_id = POWER_PROFILE_SESSION_ID;
    request->seq = ++seq;
    request->round_index = 0u;
    request->flags = POWER_PROFILE_RANGE_FLAGS;
    request->timeout_ms = POWER_PROFILE_CLICK_RX_TIMEOUT_MS;
    request->capture_rsl = POWER_PROFILE_CAPTURE_RSL != 0;
}
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_SLEEP)
static void run_clicker_sleep(void)
{
    if (radio_init_and_configure_range() < 0) {
        error_halt();
    }
    standby_or_halt();
    sleep_forever();
}
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF)
static bool click_button_is_pressed(void)
{
    int value = gpio_pin_get_raw(click_button.port, click_button.pin);

    return value == 0;
}

static void clear_click_button_latch(void)
{
#if defined(NRF_GPIO_LATCH_PRESENT)
    nrf_gpio_pin_latch_clear(NRF_GPIO_PIN_MAP(0u, CLICK_BUTTON_PIN));
#endif
}

static bool wait_for_click_button_release(void)
{
    int64_t deadline_ms = k_uptime_get() + POWER_PROFILE_BUTTON_RELEASE_TIMEOUT_MS;
    int64_t released_since_ms = -1;

    while (k_uptime_get() < deadline_ms) {
        int64_t now_ms = k_uptime_get();

        if (click_button_is_pressed()) {
            released_since_ms = -1;
        } else {
            if (released_since_ms < 0) {
                released_since_ms = now_ms;
            }
            if ((now_ms - released_since_ms) >=
                POWER_PROFILE_BUTTON_RELEASE_STABLE_MS) {
                return true;
            }
        }
        sleep_ms(POWER_PROFILE_BUTTON_RELEASE_POLL_MS);
    }
    return !click_button_is_pressed();
}

static void configure_click_button_input_or_poweroff(void)
{
    int ret;

    if (!gpio_is_ready_dt(&click_button)) {
        sys_poweroff();
    }

    ret = gpio_pin_configure_dt(&click_button, GPIO_INPUT);
    if (ret < 0) {
        sys_poweroff();
    }
    (void)gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_DISABLE);
}

static void poweroff_with_click_wake(void)
{
    int ret;

    configure_click_button_input_or_poweroff();
    if (!wait_for_click_button_release()) {
        (void)gpio_pin_configure_dt(&click_button, GPIO_DISCONNECTED);
        sys_poweroff();
    }
    clear_click_button_latch();

    ret = gpio_pin_interrupt_configure(click_button.port,
                                       click_button.pin,
                                       GPIO_INT_LEVEL_LOW);
    if (ret < 0) {
        sys_poweroff();
    }

    sys_poweroff();
}

static int send_click_wake_frame(void)
{
    uint8_t frame[UWB_WAKE_CLAIM_LEN];
    struct uwb_wake_claim_frame claim = {0};
    size_t frame_len = 0u;
    uint32_t event_id = k_uptime_get_32();
    uint64_t nonce;

    if (event_id == 0u) {
        event_id = 1u;
    }
    nonce = POWER_PROFILE_SESSION_NONCE ^
            ((uint64_t)event_id << 32) ^
            POWER_PROFILE_CLICKER_ID;
    if (nonce == 0u) {
        nonce = POWER_PROFILE_SESSION_NONCE;
    }

    claim.network_id = POWER_PROFILE_NETWORK_ID;
    claim.clicker_id = POWER_PROFILE_CLICKER_ID;
    claim.click_event_id = event_id;
    claim.attempt_index = 1u;
    claim.priority_id = POWER_PROFILE_CLICKER_ID;
    claim.wake_channel = UWB_CHANNEL_WAKE_CONTACT;
    claim.ranging_channel = UWB_CHANNEL_WAKE_CONTACT;
    claim.wake_train_ends_in_ms = POWER_PROFILE_WAKE_CLAIM_TIMING_MS;
    claim.discovery_starts_in_ms = POWER_PROFILE_WAKE_CLAIM_TIMING_MS;
    claim.claimed_duration_ms = POWER_PROFILE_WAKE_CLAIM_DURATION_MS;
    claim.min_anchor_count = UWB_NORMAL_CLICK_MIN_ANCHORS;
    claim.max_anchor_count = UWB_RANGE_SCHEDULE_MAX_ANCHORS;
    claim.nonce = nonce;
    claim.flags = POWER_PROFILE_RANGE_FLAGS;

    if (dwm3000_driver_configure_wake_mode() < 0) {
        return -EIO;
    }
    if (uwb_encode_wake_claim(&claim, frame, sizeof(frame), &frame_len) != PROTO_OK) {
        return -EINVAL;
    }
    if (dwm3000_driver_send_frame(frame, frame_len,
                                  POWER_PROFILE_WAKE_TX_TIMEOUT_MS) < 0) {
        return -EIO;
    }
    return 0;
}

static void run_clicker_systemoff(void)
{
    int ret;
    bool clicked = reset_reason_was_systemoff();

    configure_click_button_input_or_poweroff();
    if (clicked) {
        if (!wait_for_click_button_release()) {
            park_for_systemoff(false);
            sys_poweroff();
        }
        clear_click_button_latch();
    }

    if (!clicked) {
        sleep_ms(POWER_PROFILE_BOOT_SETTLE_MS);
    }
    ret = radio_init_and_configure_range();
    if (ret == 0 && clicked) {
        (void)send_click_wake_frame();
    }
    if (ret == 0) {
        (void)dwm3000_driver_standby();
    }
    park_for_systemoff(true);

    poweroff_with_click_wake();
}
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF_NOWAKE)
static void run_clicker_systemoff_nowake(void)
{
    sleep_ms(POWER_PROFILE_BOOT_SETTLE_MS);
    if (radio_init_and_configure_range() < 0) {
        error_halt();
    }
    standby_or_halt();
    park_for_systemoff(false);
    sys_poweroff();
}
#endif

#if defined(POWER_PROFILE_MODE_ANCHOR_SLEEP)
static void run_anchor_sleep(void)
{
    if (radio_init_and_configure_range() < 0) {
        error_halt();
    }
    standby_or_halt();
    sleep_forever();
}
#endif

#if defined(POWER_PROFILE_MODE_ANCHOR_LOW_DUTY)
static void run_anchor_low_duty(void)
{
    uint8_t frame[POWER_PROFILE_FRAME_BUF_LEN];
#if POWER_PROFILE_DEBUG_RTT != 0
    uint32_t scan_count = 0u;
#endif
    int ret;

    PROFILE_DBG("power_profile anchor_low_duty debug boot interval_ms=%u rx_window_ms=%u boot_settle_ms=%u\n",
                (unsigned int)POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS,
                (unsigned int)POWER_PROFILE_ANCHOR_RX_WINDOW_MS,
                (unsigned int)POWER_PROFILE_BOOT_SETTLE_MS);

    ret = radio_init_and_configure_range();
    if (ret < 0) {
        PROFILE_DBG("anchor_low_duty radio_init failed ret=%d\n", ret);
        error_halt();
    }
    ret = dwm3000_driver_configure_wake_mode();
    if (ret < 0) {
        PROFILE_DBG("anchor_low_duty initial wake_mode failed ret=%d\n", ret);
        error_halt();
    }
    PROFILE_DBG("anchor_low_duty initial wake_mode ok\n");
    standby_or_halt();
    PROFILE_DBG("anchor_low_duty radio standby ok\n");

    while (true) {
        int64_t start_ms = k_uptime_get();
        size_t frame_len = 0u;
        uint8_t quality = 0u;
        int8_t rsl_dbm = 0;
        int64_t elapsed_ms;
        uint32_t sleep_due_ms = 0u;
#if POWER_PROFILE_DEBUG_RTT != 0
        uint32_t scan = ++scan_count;
#endif

        PROFILE_DBG("anchor_low_duty scan=%u start_ms=%lld\n",
                    (unsigned int)scan, (long long)start_ms);
        marker_set(true);
        ret = dwm3000_driver_configure_wake_mode();
        if (ret < 0) {
            PROFILE_DBG("anchor_low_duty scan=%u wake_mode ret=%d\n",
                        (unsigned int)scan, ret);
            error_halt();
        }
        ret = dwm3000_driver_receive_frame(POWER_PROFILE_ANCHOR_RX_WINDOW_MS,
                                           frame, sizeof(frame), &frame_len,
                                           &quality, &rsl_dbm);
        standby_or_halt();
        marker_set(false);

        elapsed_ms = k_uptime_get() - start_ms;
        if (elapsed_ms < (int64_t)POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS) {
            sleep_due_ms =
                (uint32_t)((int64_t)POWER_PROFILE_ANCHOR_SCAN_INTERVAL_MS -
                           elapsed_ms);
        }
        PROFILE_DBG("anchor_low_duty scan=%u rx_ret=%d frame_len=%u quality=%u rsl_dbm=%d elapsed_ms=%lld sleep_ms=%u\n",
                    (unsigned int)scan, ret, (unsigned int)frame_len,
                    (unsigned int)quality, (int)rsl_dbm,
                    (long long)elapsed_ms, (unsigned int)sleep_due_ms);
        if (sleep_due_ms > 0u) {
            sleep_ms(sleep_due_ms);
        }
    }
}
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_CLICK)
static const uint64_t responder_ids[] = {
    POWER_PROFILE_RESPONDER0_ID,
    POWER_PROFILE_RESPONDER1_ID,
    POWER_PROFILE_RESPONDER2_ID,
    POWER_PROFILE_RESPONDER3_ID,
};

static void run_clicker_click(void)
{
    if (radio_init_and_configure_range() < 0) {
        error_halt();
    }
    standby_or_halt();

    while (true) {
        int64_t start_ms = k_uptime_get();
        int64_t elapsed_ms;

        marker_set(true);
        if (dwm3000_driver_configure_range_mode() < 0) {
            error_halt();
        }
        for (size_t i = 0u; i < POWER_PROFILE_TARGET_COUNT; ++i) {
            struct dwm3000_range_request request;
            struct dwm3000_range_result result;

            fill_range_request(&request, responder_ids[i]);
            memset(&result, 0, sizeof(result));
            result.status = RANGE_RX_TIMEOUT;
            (void)dwm3000_driver_range_initiator(&request, &result);

            if (POWER_PROFILE_INTER_TARGET_GAP_MS > 0u &&
                i + 1u < POWER_PROFILE_TARGET_COUNT) {
                sleep_ms(POWER_PROFILE_INTER_TARGET_GAP_MS);
            }
        }
        standby_or_halt();
        marker_set(false);

        elapsed_ms = k_uptime_get() - start_ms;
        if (elapsed_ms < (int64_t)POWER_PROFILE_CLICK_PERIOD_MS) {
            sleep_ms((uint32_t)((int64_t)POWER_PROFILE_CLICK_PERIOD_MS -
                                elapsed_ms));
        }
    }
}
#endif

#if defined(POWER_PROFILE_MODE_RESPONDER)
static void run_responder(void)
{
    if (radio_init_and_configure_range() < 0) {
        error_halt();
    }

    while (true) {
        struct dwm3000_range_request expected;
        struct dwm3000_range_result result;

        fill_range_request(&expected, POWER_PROFILE_RESPONDER0_ID);
        expected.seq = 0u;
        expected.timeout_ms = POWER_PROFILE_RESPONDER_RX_WINDOW_MS;
        memset(&result, 0, sizeof(result));
        result.status = RANGE_RX_TIMEOUT;

        marker_set(true);
        (void)dwm3000_driver_responder_poll_expected(POWER_PROFILE_RESPONDER0_ID,
                                                     &expected,
                                                     POWER_PROFILE_RESPONDER_RX_WINDOW_MS,
                                                     &result);
        marker_set(false);
    }
}
#endif

int main(void)
{
    configure_quiet_gpios();
#if !defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF) && \
    !defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF_NOWAKE)
    sleep_ms(POWER_PROFILE_BOOT_SETTLE_MS);
#endif

#if defined(POWER_PROFILE_MODE_CLICKER_SLEEP)
    run_clicker_sleep();
#elif defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF)
    run_clicker_systemoff();
#elif defined(POWER_PROFILE_MODE_CLICKER_SYSTEMOFF_NOWAKE)
    run_clicker_systemoff_nowake();
#elif defined(POWER_PROFILE_MODE_ANCHOR_SLEEP)
    run_anchor_sleep();
#elif defined(POWER_PROFILE_MODE_ANCHOR_LOW_DUTY)
    run_anchor_low_duty();
#elif defined(POWER_PROFILE_MODE_CLICKER_CLICK)
    run_clicker_click();
#elif defined(POWER_PROFILE_MODE_RESPONDER)
    run_responder();
#else
#error "A POWER_PROFILE_MODE_* definition is required"
#endif

    return 0;
}
