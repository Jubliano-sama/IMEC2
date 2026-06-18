#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#define BLINK_ON_MS 200
#define BLINK_OFF_MS 200
#define GAP_MS 700

static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(status0_red), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(status0_green), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(status0_blue), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(status1_red), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(status1_green), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(status1_blue), gpios),
};

enum {
    LED_0_RED,
    LED_0_GREEN,
    LED_0_BLUE,
    LED_1_RED_P008,
    LED_1_GREEN,
    LED_1_BLUE,
};

static void all_leds_off(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
        gpio_pin_set_dt(&leds[i], 0);
    }
}

static void blink_led(size_t index, int repeats)
{
    for (int i = 0; i < repeats; ++i) {
        all_leds_off();
        gpio_pin_set_dt(&leds[index], 1);
        k_msleep(BLINK_ON_MS);
        gpio_pin_set_dt(&leds[index], 0);
        k_msleep(BLINK_OFF_MS);
    }
}

int main(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
        if (!gpio_is_ready_dt(&leds[i])) {
            return 1;
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(leds); ++i) {
        int err = gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE);

        if (err != 0) {
            return 1;
        }
    }

    while (true) {
        blink_led(LED_1_RED_P008, 4);
        k_msleep(GAP_MS);
        blink_led(LED_1_GREEN, 2);
        blink_led(LED_1_BLUE, 2);
        blink_led(LED_0_RED, 1);
        blink_led(LED_0_GREEN, 1);
        blink_led(LED_0_BLUE, 1);
        k_msleep(GAP_MS);
    }

    return 0;
}
