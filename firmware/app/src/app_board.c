#include "app_config.h"
#include "app_board.h"
#include "app_state.h"
#include "uwb.h"

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_USE_SEGGER_RTT)
#include <SEGGER_RTT.h>
#endif

#include <errno.h>
#include <stdarg.h>

LOG_MODULE_REGISTER(app_board, LOG_LEVEL_DBG);

#define DEBUG_LED_PULSE_MS 250u
#define DEBUG_CH5_RX_PULSE_MS 1000u
#define DEBUG_CH5_TX_PULSE_MS 400u
#define DEBUG_TX_BOOT_TEST_MS 600u

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
static const struct gpio_dt_spec status0_red = GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
static const struct gpio_dt_spec status0_green = GPIO_DT_SPEC_GET(STATUS0_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
static const struct gpio_dt_spec status0_blue = GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
static const struct gpio_dt_spec status1_red = GPIO_DT_SPEC_GET(STATUS1_RED_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
static const struct gpio_dt_spec status1_green = GPIO_DT_SPEC_GET(STATUS1_GREEN_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
static const struct gpio_dt_spec status1_blue = GPIO_DT_SPEC_GET(STATUS1_BLUE_NODE, gpios);
#endif
#if DT_NODE_HAS_STATUS(BATTERY_ADC_ENABLE_NODE, okay)
static const struct gpio_dt_spec battery_adc_enable =
    GPIO_DT_SPEC_GET(BATTERY_ADC_ENABLE_NODE, gpios);
#endif
#if defined(CONFIG_ADC) && DT_NODE_HAS_STATUS(BATTERY_ADC_NODE, okay)
static const struct adc_dt_spec battery_adc = {
    .dev = DEVICE_DT_GET(DT_PARENT(BATTERY_ADC_NODE)),
    .channel_id = DT_REG_ADDR(BATTERY_ADC_NODE),
    .channel_cfg_dt_node_exists = true,
    .channel_cfg = ADC_CHANNEL_CFG_DT(BATTERY_ADC_NODE),
    .vref_mv = DT_PROP_OR(BATTERY_ADC_NODE, zephyr_vref_mv, 0),
    .resolution = DT_PROP_OR(BATTERY_ADC_NODE, zephyr_resolution, 0),
    .oversampling = DT_PROP_OR(BATTERY_ADC_NODE, zephyr_oversampling, 0),
};
#define HAS_BATTERY_ADC 1
#else
#define HAS_BATTERY_ADC 0
#endif

static struct k_work_delayable status1_debug_pulse_restore_work;
static struct k_work_delayable status0_debug_pulse_restore_work;
static struct k_work_delayable status0_power_blink_work;
static bool status1_debug_pulse_work_ready;
static bool status0_debug_pulse_work_ready;
static bool status0_power_blink_work_ready;
static bool status_power_indicator_enabled;
static bool status0_power_red_on;
static bool status0_debug_pulse_active;
static uint32_t status0_ch5_rx_hold_until_ms;

static int BLE_CONNECTIVITY_TEST_UNUSED configure_output(const struct gpio_dt_spec *gpio)
{
    if (!gpio_is_ready_dt(gpio)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
}

int battery_adc_divider_disable(void)
{
#if DT_NODE_HAS_STATUS(BATTERY_ADC_ENABLE_NODE, okay)
    int ret;

    if (!gpio_is_ready_dt(&battery_adc_enable)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure(battery_adc_enable.port, battery_adc_enable.pin,
                             GPIO_OUTPUT_HIGH);
    if (ret < 0) {
        return ret;
    }
    return gpio_pin_set_raw(battery_adc_enable.port, battery_adc_enable.pin, 1);
#else
    return 0;
#endif
}

static int BLE_CONNECTIVITY_TEST_UNUSED battery_adc_divider_enable(void)
{
#if DT_NODE_HAS_STATUS(BATTERY_ADC_ENABLE_NODE, okay)
    int ret;

    if (!gpio_is_ready_dt(&battery_adc_enable)) {
        return -ENODEV;
    }

    ret = gpio_pin_configure(battery_adc_enable.port, battery_adc_enable.pin,
                             GPIO_OUTPUT_LOW);
    if (ret < 0) {
        return ret;
    }
    return gpio_pin_set_raw(battery_adc_enable.port, battery_adc_enable.pin, 0);
#else
    return 0;
#endif
}

int battery_sample_lithium_mv(uint16_t *battery_mv)
{
#if HAS_BATTERY_ADC
    int16_t raw_sample = 0;
    int32_t adc_mv;
    struct adc_sequence sequence = {
        .buffer = &raw_sample,
        .buffer_size = sizeof(raw_sample),
    };
    int ret;

    if (battery_mv == NULL) {
        return -EINVAL;
    }
    if (!adc_is_ready_dt(&battery_adc)) {
        return -ENODEV;
    }

    ret = battery_adc_divider_enable();
    if (ret < 0) {
        return ret;
    }
    k_msleep(6);

    ret = adc_channel_setup_dt(&battery_adc);
    if (ret < 0) {
        (void)battery_adc_divider_disable();
        return ret;
    }
    ret = adc_sequence_init_dt(&battery_adc, &sequence);
    if (ret < 0) {
        (void)battery_adc_divider_disable();
        return ret;
    }
    ret = adc_read_dt(&battery_adc, &sequence);
    if (ret < 0) {
        (void)battery_adc_divider_disable();
        return ret;
    }
    (void)battery_adc_divider_disable();

    adc_mv = raw_sample;
    ret = adc_raw_to_millivolts_dt(&battery_adc, &adc_mv);
    if (ret < 0) {
        return ret;
    }
    if (adc_mv < 0) {
        adc_mv = 0;
    }
    adc_mv *= 2;
    *battery_mv = (uint16_t)MIN(adc_mv, (int32_t)UINT16_MAX);
    return 0;
#else
    ARG_UNUSED(battery_mv);
    return -ENODEV;
#endif
}

static void BLE_CONNECTIVITY_TEST_UNUSED set_output(const struct gpio_dt_spec *gpio, bool enabled)
{
    if (gpio_is_ready_dt(gpio)) {
        (void)gpio_pin_set_dt(gpio, enabled ? 1 : 0);
    }
}

static bool reserve_status1_for_power_indicator(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST);
}

static bool reserve_status0_for_route_test_power(void)
{
    return IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST);
}

static void status0_route_test_power_apply(void)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !status_power_indicator_enabled) {
        return;
    }

    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        status_led0_set(status0_power_red_on, false, false);
    } else if (DEVICE_ROLE == ROLE_GATEWAY) {
        status_led0_set(true, false, false);
    } else if (DEVICE_ROLE == ROLE_ANCHOR) {
        status_led0_set(false, false, true);
    }
}

void status_leds_set(bool red, bool green, bool blue)
{
    if (!reserve_status0_for_route_test_power()) {
        status_led0_set(red, green, blue);
    }
    if (reserve_status1_for_power_indicator()) {
        return;
    }
    status_led1_set(red, green, blue);
}

void status_led0_set(bool red, bool green, bool blue)
{
#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    set_output(&status0_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    set_output(&status0_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    set_output(&status0_blue, blue);
#endif
}

void status_led1_set(bool red, bool green, bool blue)
{
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    set_output(&status1_red, red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    set_output(&status1_green, green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    set_output(&status1_blue, blue);
#endif
}

void status_power_indicator_set(bool enabled)
{
    status_power_indicator_enabled = enabled;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        if (enabled) {
            status0_route_test_power_apply();
        } else {
            status_led0_set(false, false, false);
        }
    } else {
        status_led1_set(false, false, enabled);
    }
}

static void status1_debug_pulse_restore_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    ARG_UNUSED(status_power_indicator_enabled);
    status_led1_set(false, false, false);
}

static void status0_debug_pulse_restore_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    status0_debug_pulse_active = false;
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
        status_power_indicator_enabled) {
        status0_route_test_power_apply();
        return;
    }
    status_led0_set(false, false, false);
}

static void status0_power_blink_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!status0_power_blink_work_ready ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return;
    }
    if (status0_debug_pulse_active) {
        (void)k_work_reschedule(&status0_power_blink_work, K_MSEC(DEBUG_LED_PULSE_MS));
        return;
    }
    status0_power_red_on = !status0_power_red_on;
    status0_route_test_power_apply();
    (void)k_work_reschedule(&status0_power_blink_work, K_MSEC(500));
}

static void status1_debug_pulse(bool red, bool green, bool blue)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !status1_debug_pulse_work_ready) {
        return;
    }

    status_led1_set(red, green, blue);
    (void)k_work_reschedule(&status1_debug_pulse_restore_work,
                            K_MSEC(DEBUG_LED_PULSE_MS));
}

static void status0_debug_pulse_for(bool red,
                                    bool green,
                                    bool blue,
                                    uint32_t duration_ms)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        !status0_debug_pulse_work_ready) {
        return;
    }

    status0_debug_pulse_active = true;
    status_led0_set(red, green, blue);
    (void)k_work_reschedule(&status0_debug_pulse_restore_work,
                            K_MSEC(duration_ms));
}

static bool anchor_route_test_activity_leds_enabled(void)
{
    return DEVICE_ROLE == ROLE_ANCHOR &&
           IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) &&
           !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER);
}

static void status1_debug_ch9_rx_pulse(void)
{
    status1_debug_pulse(false, true, false);
}

static void status1_debug_ch9_tx_pulse(void)
{
    status1_debug_pulse(false, false, true);
}

static void status0_debug_ch5_rx_pulse(void)
{
    status0_ch5_rx_hold_until_ms = k_uptime_get_32() + DEBUG_CH5_RX_PULSE_MS;
    status0_debug_pulse_for(false, true, false, DEBUG_CH5_RX_PULSE_MS);
}

static void status0_debug_ch5_boot_selftest_pulse(void)
{
    status0_debug_pulse_for(false, true, false, DEBUG_CH5_RX_PULSE_MS);
    status0_ch5_rx_hold_until_ms = 0u;
}

static void status0_debug_ch5_tx_pulse(void)
{
    uint32_t now_ms = k_uptime_get_32();

    if (!uptime_deadline_reached(now_ms, status0_ch5_rx_hold_until_ms)) {
        return;
    }
    if (anchor_route_test_activity_leds_enabled()) {
        status0_debug_pulse_for(true, false, false, DEBUG_CH5_TX_PULSE_MS);
    } else {
        status0_debug_pulse_for(false, false, true, DEBUG_CH5_TX_PULSE_MS);
    }
}

static void status_debug_unknown_channel_pulse(uint8_t uwb_channel)
{
    if (uwb_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        status1_debug_ch9_rx_pulse();
    } else if (anchor_route_test_activity_leds_enabled() &&
               uwb_channel == UWB_CHANNEL_WAKE_CONTACT) {
        status0_debug_ch5_rx_pulse();
    } else if (uwb_channel == UWB_CHANNEL_WAKE_CONTACT) {
        status0_debug_ch5_rx_pulse();
    } else {
        status1_debug_pulse(true, false, false);
    }
}

static void debug_rtt_write(const char *text)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        ARG_UNUSED(text);
        return;
    }
#if defined(CONFIG_USE_SEGGER_RTT)
    (void)SEGGER_RTT_WriteString(0, text);
#else
    ARG_UNUSED(text);
#endif
}

void status_debug_note(const char *text)
{
    if (text == NULL) {
        return;
    }
    debug_rtt_write(text);
}

void status_debug_printf(const char *fmt, ...)
{
#if defined(CONFIG_USE_SEGGER_RTT)
    char line[128];
    va_list args;
    int ret;
    size_t len;

    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) || fmt == NULL) {
        return;
    }

    va_start(args, fmt);
    ret = vsnprintk(line, sizeof(line), fmt, args);
    va_end(args);
    if (ret <= 0) {
        return;
    }

    len = MIN((size_t)ret, sizeof(line) - 1u);
    (void)SEGGER_RTT_Write(0, line, len);
#else
    ARG_UNUSED(fmt);
#endif
}

void status_debug_uwb_rx_channel_pulse(uint8_t uwb_channel)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    if (uwb_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        status1_debug_ch9_rx_pulse();
        debug_rtt_write("DBG_UWB_RX_CH9\n");
    } else if (uwb_channel == UWB_CHANNEL_WAKE_CONTACT) {
        status0_debug_ch5_rx_pulse();
        debug_rtt_write("DBG_UWB_RX_CH5\n");
    } else {
        status_debug_unknown_channel_pulse(uwb_channel);
        debug_rtt_write("DBG_UWB_RX_UNKNOWN_CH\n");
    }
}

void status_debug_gateway_uwb_rx_channel_pulse(uint8_t uwb_channel)
{
    status_debug_uwb_rx_channel_pulse(uwb_channel);
}

void status_debug_uwb_tx_channel_pulse(uint8_t uwb_channel)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    if (uwb_channel == UWB_CHANNEL_MESH_PAYLOAD) {
        status1_debug_ch9_tx_pulse();
        debug_rtt_write("DBG_UWB_TX_CH9\n");
    } else if (uwb_channel == UWB_CHANNEL_WAKE_CONTACT) {
        status0_debug_ch5_tx_pulse();
        debug_rtt_write("DBG_UWB_TX_CH5\n");
    } else {
        status_debug_unknown_channel_pulse(uwb_channel);
        debug_rtt_write("DBG_UWB_TX_UNKNOWN_CH\n");
    }
}

void status_debug_tx_boot_test(void)
{
    if (!IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return;
    }

    ARG_UNUSED(DEBUG_TX_BOOT_TEST_MS);
    status0_power_red_on = true;
    status_power_indicator_set(true);
    status0_debug_ch5_boot_selftest_pulse();
    debug_rtt_write("DBG_LED_SELFTEST_CH5_RX\n");
    if (status0_power_blink_work_ready) {
        (void)k_work_reschedule(&status0_power_blink_work, K_MSEC(500));
    }
}

void status_debug_gateway_boot_test(void)
{
    if (DEVICE_ROLE != ROLE_GATEWAY || !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        return;
    }

    ARG_UNUSED(DEBUG_TX_BOOT_TEST_MS);
    status0_power_red_on = true;
    status_power_indicator_set(true);
    status0_debug_ch5_boot_selftest_pulse();
    debug_rtt_write("DBG_LED_SELFTEST_CH5_RX\n");
}

void status_debug_anchor_boot_test(void)
{
    if (DEVICE_ROLE != ROLE_ANCHOR ||
        !IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST) ||
        IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        return;
    }

    status_power_indicator_set(true);
    status0_debug_ch5_boot_selftest_pulse();
    debug_rtt_write("DBG_LED_SELFTEST_CH5_RX\n");
}

void status_debug_tx_packet_sent_pulse(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        debug_rtt_write("DBG_TX_PACKET_SENT\n");
    }
}

void status_debug_tx_wake_claim_sent_pulse(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        status_debug_uwb_tx_channel_pulse(UWB_CHANNEL_WAKE_CONTACT);
        debug_rtt_write("DBG_TX_WAKE_CLAIM_SENT\n");
    }
}

void status_debug_tx_mesh_frame_sent_pulse(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        debug_rtt_write("DBG_TX_MESH_FRAME_SENT\n");
    }
}

void status_debug_tx_gateway_ack_rx_pulse(void)
{
    if (IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST_TRANSMITTER)) {
        status1_debug_pulse(false, true, false);
    }
}

void status_debug_gateway_ack_tx_pulse(void)
{
    if (DEVICE_ROLE == ROLE_GATEWAY && IS_ENABLED(CONFIG_IMEC_MESH_ROUTE_TEST)) {
        debug_rtt_write("DBG_GATEWAY_ACK_TX_LED\n");
    }
}

static void BLE_CONNECTIVITY_TEST_UNUSED disconnect_gpio(const struct gpio_dt_spec *gpio)
{
    if (gpio_is_ready_dt(gpio)) {
        (void)gpio_pin_configure_dt(gpio, GPIO_DISCONNECTED);
    }
}

void status_leds_disconnect(void)
{
#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    disconnect_gpio(&status0_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    disconnect_gpio(&status0_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    disconnect_gpio(&status0_blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    disconnect_gpio(&status1_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    disconnect_gpio(&status1_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    disconnect_gpio(&status1_blue);
#endif
}

void status_apply(const struct status_inputs *inputs)
{
    struct status_indication indication;

    if (status_select(inputs, &indication) != PROTO_OK) {
        return;
    }

    switch (indication.pattern) {
    case STATUS_PATTERN_BLUE_PULSE:
    case STATUS_PATTERN_BLUE_CHASE:
        status_leds_set(false, false, true);
        break;
    case STATUS_PATTERN_GREEN_SOLID:
    case STATUS_PATTERN_GREEN_SLOW_BLINK:
        status_leds_set(false, true, false);
        break;
    case STATUS_PATTERN_AMBER_BLINK_ONCE:
    case STATUS_PATTERN_AMBER_SLOW_BLINK:
        status_leds_set(true, true, false);
        break;
    case STATUS_PATTERN_RED_BLINK_CODE:
        status_leds_set(true, false, false);
        break;
    case STATUS_PATTERN_OFF:
    default:
        status_leds_set(false, false, false);
        break;
    }

    LOG_INF("status pattern=%d red_blinks=%u repeat=%u duration_ms=%u",
            indication.pattern, indication.red_blink_count,
            indication.repeat_count, indication.duration_ms);
}

int status_leds_init(void)
{
    int ret = 0;

    k_work_init_delayable(&status1_debug_pulse_restore_work,
                          status1_debug_pulse_restore_handler);
    status1_debug_pulse_work_ready = true;
    k_work_init_delayable(&status0_debug_pulse_restore_work,
                          status0_debug_pulse_restore_handler);
    status0_debug_pulse_work_ready = true;
    k_work_init_delayable(&status0_power_blink_work,
                          status0_power_blink_handler);
    status0_power_blink_work_ready = true;

#if DT_NODE_HAS_STATUS(STATUS0_RED_NODE, okay)
    ret |= configure_output(&status0_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_GREEN_NODE, okay)
    ret |= configure_output(&status0_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS0_BLUE_NODE, okay)
    ret |= configure_output(&status0_blue);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_RED_NODE, okay)
    ret |= configure_output(&status1_red);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_GREEN_NODE, okay)
    ret |= configure_output(&status1_green);
#endif
#if DT_NODE_HAS_STATUS(STATUS1_BLUE_NODE, okay)
    ret |= configure_output(&status1_blue);
#endif

    status_led0_set(false, false, false);
    if (reserve_status1_for_power_indicator()) {
        status_power_indicator_set(true);
        status_led1_set(false, false, false);
    } else {
        status_led1_set(false, false, false);
    }
    return ret;
}

int debug_serial_init(void)
{
    /* Runtime diagnostics are BLE or RTT only on current hardware. */
    return 0;
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
bool debug_serial_dtr_ready(void)
{
    return false;
}

int debug_serial_poll_in(unsigned char *byte)
{
    ARG_UNUSED(byte);
    return -ENODEV;
}
#endif

int app_board_init(void)
{
    return status_leds_init();
}
