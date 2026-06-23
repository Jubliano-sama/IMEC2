#include "app_config.h"
#include "app_board.h"
#include "app_state.h"

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_USB_DEVICE_STACK)
#include <zephyr/usb/usb_device.h>
#endif

#include <errno.h>

LOG_MODULE_REGISTER(app_board, LOG_LEVEL_DBG);

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

#if defined(CONFIG_USB_DEVICE_STACK) && DT_NODE_HAS_STATUS(USB_CONSOLE_NODE, okay)
static const struct device *serial_console = DEVICE_DT_GET(USB_CONSOLE_NODE);
#define HAS_SERIAL_CONSOLE 1
#else
#define HAS_SERIAL_CONSOLE 0
#endif

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

void status_leds_set(bool red, bool green, bool blue)
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

    status_leds_set(false, false, false);
    return ret;
}

int debug_serial_init(void)
{
    if (gateway_ble_transport_enabled()) {
        return 0;
    }
#if HAS_SERIAL_CONSOLE
    if (!device_is_ready(serial_console)) {
        return -ENODEV;
    }
#endif
#if defined(CONFIG_USB_DEVICE_STACK)
    int ret = usb_enable(NULL);

    if (ret < 0 && ret != -EALREADY) {
        return ret;
    }
#endif
    return 0;
}

#if defined(CONFIG_IMEC_HIGH_DEBUG)
bool debug_serial_dtr_ready(void)
{
#if HAS_SERIAL_CONSOLE && defined(CONFIG_UART_LINE_CTRL)
    uint32_t dtr = 0u;

    if (uart_line_ctrl_get(serial_console, UART_LINE_CTRL_DTR, &dtr) == 0) {
        return dtr != 0u;
    }
#endif
    return true;
}

int debug_serial_poll_in(unsigned char *byte)
{
#if HAS_SERIAL_CONSOLE
    if (byte == NULL) {
        return -EINVAL;
    }
    if (!device_is_ready(serial_console) || !debug_serial_dtr_ready()) {
        return -ENODEV;
    }
    return uart_poll_in(serial_console, byte);
#else
    ARG_UNUSED(byte);
    return -ENODEV;
#endif
}
#endif

int app_board_init(void)
{
    return status_leds_init();
}
