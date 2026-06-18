#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <stddef.h>

#define PWM_PERIOD_US 2000u
#define PWM_STEPS 100u
#define STEP_HOLD_MS 18u
#define FULL_HOLD_MS 250u
#define OFF_HOLD_MS 250u

#define STATUS0_RED_NODE DT_ALIAS(status0_red)
#define STATUS0_GREEN_NODE DT_ALIAS(status0_green)
#define STATUS0_BLUE_NODE DT_ALIAS(status0_blue)
#define STATUS1_RED_NODE DT_ALIAS(status1_red)
#define STATUS1_GREEN_NODE DT_ALIAS(status1_green)
#define STATUS1_BLUE_NODE DT_ALIAS(status1_blue)
#define BATTERY_ADC_ENABLE_NODE DT_ALIAS(battery_adc_enable)

static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(STATUS0_RED_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS0_GREEN_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS0_BLUE_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_RED_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_GREEN_NODE, gpios),
    GPIO_DT_SPEC_GET(STATUS1_BLUE_NODE, gpios),
};

static const struct gpio_dt_spec battery_adc_enable =
    GPIO_DT_SPEC_GET(BATTERY_ADC_ENABLE_NODE, gpios);

static void set_all_leds(bool on)
{
    for (size_t i = 0u; i < ARRAY_SIZE(leds); ++i) {
        (void)gpio_pin_set_dt(&leds[i], on ? 1 : 0);
    }
}

static void run_pwm_period(uint32_t duty_step)
{
    uint32_t on_us = (PWM_PERIOD_US * duty_step) / PWM_STEPS;

    if (on_us == 0u) {
        set_all_leds(false);
        k_busy_wait(PWM_PERIOD_US);
        return;
    }

    if (on_us >= PWM_PERIOD_US) {
        set_all_leds(true);
        k_busy_wait(PWM_PERIOD_US);
        return;
    }

    set_all_leds(true);
    k_busy_wait(on_us);
    set_all_leds(false);
    k_busy_wait(PWM_PERIOD_US - on_us);
}

static void hold_duty(uint32_t duty_step, uint32_t hold_ms)
{
    uint32_t periods = MAX(1u, (hold_ms * 1000u) / PWM_PERIOD_US);

    for (uint32_t i = 0u; i < periods; ++i) {
        run_pwm_period(duty_step);
    }
}

static void pulse_once(void)
{
    for (uint32_t duty = 0u; duty <= PWM_STEPS; ++duty) {
        hold_duty(duty, STEP_HOLD_MS);
    }

    hold_duty(PWM_STEPS, FULL_HOLD_MS);

    for (uint32_t duty = PWM_STEPS; duty > 0u; --duty) {
        hold_duty(duty - 1u, STEP_HOLD_MS);
    }

    hold_duty(0u, OFF_HOLD_MS);
}

int main(void)
{
    for (size_t i = 0u; i < ARRAY_SIZE(leds); ++i) {
        if (!gpio_is_ready_dt(&leds[i])) {
            return 1;
        }
    }

    for (size_t i = 0u; i < ARRAY_SIZE(leds); ++i) {
        if (gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) < 0) {
            return 1;
        }
    }

    if (gpio_is_ready_dt(&battery_adc_enable)) {
        (void)gpio_pin_configure_dt(&battery_adc_enable, GPIO_OUTPUT_INACTIVE);
    }

    while (true) {
        pulse_once();
    }

    return 0;
}
