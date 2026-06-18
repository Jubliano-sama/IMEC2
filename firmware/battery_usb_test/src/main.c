#include <inttypes.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(battery_usb_test, LOG_LEVEL_INF);

#define BATTERY_ADC_NODE DT_PATH(zephyr_user)
#define BATTERY_ADC_ENABLE_NODE DT_ALIAS(battery_adc_enable)
#define BATTERY_GREEN_PWM_NODE DT_ALIAS(battery_green_pwm)
#define USB_BLUE_NODE DT_ALIAS(usb_blue)

#define BATTERY_MIN_MV 3000
#define BATTERY_MAX_MV 4200
#define SAMPLE_PERIOD_MS 100

static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET(BATTERY_ADC_NODE);
static const struct gpio_dt_spec battery_adc_enable =
	GPIO_DT_SPEC_GET(BATTERY_ADC_ENABLE_NODE, gpios);
static const struct pwm_dt_spec battery_green_pwm =
	PWM_DT_SPEC_GET(BATTERY_GREEN_PWM_NODE);
static const struct gpio_dt_spec usb_blue = GPIO_DT_SPEC_GET(USB_BLUE_NODE, gpios);

static int read_battery_mv(int32_t *battery_mv, int32_t *divider_mv, int16_t *raw_out)
{
	int16_t raw = 0;
	struct adc_sequence sequence = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int ret;
	int32_t mv;

	adc_sequence_init_dt(&battery_adc, &sequence);

	ret = adc_read_dt(&battery_adc, &sequence);
	if (ret < 0) {
		return ret;
	}

	mv = raw;
	ret = adc_raw_to_millivolts_dt(&battery_adc, &mv);
	if (ret < 0) {
		return ret;
	}

	*raw_out = raw;
	*divider_mv = mv;
	*battery_mv = mv * 2;
	return 0;
}

static uint32_t battery_pwm_pulse(int32_t battery_mv)
{
	int32_t clamped_mv = CLAMP(battery_mv, BATTERY_MIN_MV, BATTERY_MAX_MV);

	if (battery_mv <= BATTERY_MIN_MV) {
		return 0;
	}

	return (uint32_t)(((uint64_t)battery_green_pwm.period *
			   (uint32_t)(clamped_mv - BATTERY_MIN_MV)) /
			  (BATTERY_MAX_MV - BATTERY_MIN_MV));
}

static bool usb_vbus_present(void)
{
#if NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
#else
	return false;
#endif
}

static uint32_t usb_reg_status(void)
{
#if NRF_POWER_HAS_USBREG
	return nrf_power_usbregstatus_get(NRF_POWER);
#else
	return 0u;
#endif
}

int main(void)
{
	int ret;
	uint32_t log_divider = 0;

	if (!adc_is_ready_dt(&battery_adc)) {
		LOG_ERR("ADC device %s is not ready", battery_adc.dev->name);
		return 0;
	}

	if (!pwm_is_ready_dt(&battery_green_pwm)) {
		LOG_ERR("PWM device %s is not ready", battery_green_pwm.dev->name);
		return 0;
	}

	if (!gpio_is_ready_dt(&battery_adc_enable) ||
	    !gpio_is_ready_dt(&usb_blue)) {
		LOG_ERR("One or more GPIO devices are not ready");
		return 0;
	}

	ret = adc_channel_setup_dt(&battery_adc);
	if (ret < 0) {
		LOG_ERR("adc_channel_setup_dt failed: %d", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&battery_adc_enable, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("battery ADC enable configure failed: %d", ret);
		return 0;
	}

	ret = gpio_pin_configure_dt(&usb_blue, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("USB blue configure failed: %d", ret);
		return 0;
	}

	LOG_INF("battery/USB test firmware started");
	LOG_INF("green PWM=P0.14, 3.0-4.2V scale, blue=P0.17 follows nRF VBUS sense, ADC enable=P0.07 active-low");

	while (true) {
		int32_t battery_mv = 0;
		int32_t divider_mv = 0;
		int16_t raw = 0;
		bool usb_present;
		uint32_t usbreg;
		uint32_t pulse;

		ret = read_battery_mv(&battery_mv, &divider_mv, &raw);
		if (ret < 0) {
			LOG_ERR("battery ADC read failed: %d", ret);
			pulse = 0;
		} else {
			pulse = battery_pwm_pulse(battery_mv);
		}

		(void)pwm_set_pulse_dt(&battery_green_pwm, pulse);

		usbreg = usb_reg_status();
		usb_present = usb_vbus_present();
		(void)gpio_pin_set_dt(&usb_blue, usb_present ? 1 : 0);

		if ((log_divider++ % 10u) == 0u) {
			LOG_INF("battery=%" PRId32 " mV divider=%" PRId32
				" mV raw=%" PRId16
				" green=%u%% usbreg=0x%08" PRIx32 " vbus=%d blue=%d",
				battery_mv, divider_mv, raw,
				(unsigned int)((pulse * 100u) / battery_green_pwm.period),
				usbreg, usb_present ? 1 : 0, usb_present ? 1 : 0);
		}

		k_msleep(SAMPLE_PERIOD_MS);
	}
}
